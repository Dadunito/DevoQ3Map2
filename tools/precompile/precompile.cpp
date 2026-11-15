/*
===========================================================================
Precompile Shaders - DevoJK's lighting prebaker

A small window that bakes DevoGI lighting for many maps at once, outside the
game, so nobody has to sit through a bake while a map loads.

  - It looks next to itself for the game (eternaljk.x86_64.exe, base/,
    EternalJK/) and for mapcompiler.exe, which does the actual baking.
  - It lists every map it finds: inside the pk3 files in base/ and EternalJK/
    (the original game, and any map pack someone downloaded) and loose .bsp
    files in their maps/ folders.
  - Tick the maps you want, pick High spec, Low spec or Both, press Prebake.
    The maps are baked one after the other into EternalJK/maps/, with exactly
    the settings from EternalJK/eternaljk.cfg, so the game picks the files up
    as if it had baked them itself.

High spec is r_devoGI_live 1 (live sun and light sources, files named
<map>.live.devogi), low spec is r_devoGI_live 0 (everything baked, <map>.devogi).

Maps made for rend2 bring their own HDR lighting (maps/<name>/*.hdr, *.raw). The
game leaves DevoGI off for those, so they're marked in the list and left out of
"All" - baking them would only make files the game never uses.

It's one file of plain Win32 on purpose: easy to read, easy to change, nothing
to install. Build it with the rest of DevoQ3Map2 (tools/precompile).

DevoJK by Devoted, GPLv2.
===========================================================================
*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#pragma comment( lib, "comctl32.lib" )
#pragma comment( linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"" )

// ---------------------------------------------------------------------------
// Little helpers
// ---------------------------------------------------------------------------

static std::string Lower( std::string s )
{
	for ( char &c : s )
		c = (char)tolower( (unsigned char)c );
	return s;
}

static bool EndsWith( const std::string &s, const char *tail )
{
	const size_t n = strlen( tail );
	return s.size() >= n && s.compare( s.size() - n, n, tail ) == 0;
}

static bool FileExists( const std::string &path )
{
	const DWORD a = GetFileAttributesA( path.c_str() );
	return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
}

static std::string ExeFolder()
{
	char path[MAX_PATH];
	GetModuleFileNameA( NULL, path, MAX_PATH );
	std::string s = path;
	return s.substr( 0, s.find_last_of( "\\/" ) );
}

// ---------------------------------------------------------------------------
// Finding the maps
//
// A pk3 is a zip file. All we need is the list of names inside, and a zip keeps
// that list (the "central directory") at its very end, so we read just that.
// ---------------------------------------------------------------------------

static std::vector<std::string> ZipNames( const std::string &path )
{
	std::vector<std::string> names;
	FILE *f = fopen( path.c_str(), "rb" );
	if ( !f )
		return names;
	fseek( f, 0, SEEK_END );
	const long size = ftell( f );
	// the end record sits in the last 22 bytes, plus up to 64k of zip comment
	const long tail = std::min( size, 65536L + 22L );
	std::vector<unsigned char> buf( tail );
	fseek( f, size - tail, SEEK_SET );
	if ( fread( buf.data(), 1, tail, f ) != (size_t)tail ) {
		fclose( f );
		return names;
	}
	long end = -1;
	for ( long i = tail - 22; i >= 0; i-- ) {
		if ( buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06 ) {
			end = i;
			break;
		}
	}
	if ( end < 0 ) {
		fclose( f );
		return names;
	}
	auto u16 = []( const unsigned char *p ) { return (unsigned)( p[0] | ( p[1] << 8 ) ); };
	auto u32 = []( const unsigned char *p ) { return (unsigned)( p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (unsigned)p[3] << 24 ) ); };
	const unsigned count = u16( &buf[end + 10] );
	const unsigned dirSize = u32( &buf[end + 12] );
	const unsigned dirOffset = u32( &buf[end + 16] );

	std::vector<unsigned char> dir( dirSize );
	fseek( f, (long)dirOffset, SEEK_SET );
	if ( fread( dir.data(), 1, dirSize, f ) != dirSize ) {
		fclose( f );
		return names;
	}
	fclose( f );

	size_t p = 0;
	for ( unsigned i = 0; i < count && p + 46 <= dir.size(); i++ ) {
		if ( u32( &dir[p] ) != 0x02014b50 )
			break;
		const unsigned nameLen = u16( &dir[p + 28] ), extraLen = u16( &dir[p + 30] ), commentLen = u16( &dir[p + 32] );
		if ( p + 46 + nameLen > dir.size() )
			break;
		names.emplace_back( (const char *)&dir[p + 46], nameLen );
		p += 46 + nameLen + extraLen + commentLen;
	}
	return names;
}

struct MapEntry {
	std::string bsp;		// path inside the game, e.g. maps/mp/ffa3.bsp
	std::string source;		// where it was found, e.g. assets0.pk3
	bool rend2 = false;		// brings its own rend2 lighting (see IsRend2)
};

// every file name seen in the pk3s and folders, lower case (for the rend2 check)
static std::vector<std::string> g_allFiles;

/*
rend2 keeps a map's own lighting next to it, in maps/<map name>/ (just the name,
no mp/): HDR lightmaps (lm_0000.hdr), the light grid and vertex light (.raw), and
reflection probes in cubemaps/<map name>/. The game's renderer checks the same.
*/
static bool IsRend2( const std::string &bsp )
{
	std::string name = Lower( bsp.substr( 0, bsp.size() - 4 ) );
	name = name.substr( name.find_last_of( '/' ) + 1 );
	const std::string dir = "maps/" + name + "/", probes = "cubemaps/" + name + "/";
	for ( const std::string &f : g_allFiles ) {
		if ( f.compare( 0, dir.size(), dir ) == 0 && ( EndsWith( f, ".hdr" ) || EndsWith( f, ".raw" ) ) )
			return true;
		if ( f.compare( 0, probes.size(), probes ) == 0 )
			return true;
	}
	return false;
}

static void FindLooseMaps( const std::string &root, const std::string &rel, std::map<std::string, MapEntry> &out, const std::string &label )
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA( ( root + "\\" + rel + "\\*" ).c_str(), &fd );
	if ( h == INVALID_HANDLE_VALUE )
		return;
	do {
		const std::string name = fd.cFileName;
		if ( name == "." || name == ".." )
			continue;
		const std::string sub = rel + "/" + name;
		if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
			FindLooseMaps( root, sub, out, label );
			continue;
		}
		g_allFiles.push_back( Lower( sub ) );
		if ( EndsWith( Lower( name ), ".bsp" ) ) {
			const std::string key = Lower( sub );
			if ( !out.count( key ) )
				out[key] = { sub, label };
		}
	} while ( FindNextFileA( h, &fd ) );
	FindClose( h );
}

// every map in base/ and EternalJK/, pk3s and loose files; later folders win, like in the game
static std::vector<MapEntry> FindMaps( const std::string &game )
{
	std::map<std::string, MapEntry> found;
	for ( const char *folder : { "base", "EternalJK" } ) {
		const std::string dir = game + "\\" + folder;
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA( ( dir + "\\*.pk3" ).c_str(), &fd );
		if ( h != INVALID_HANDLE_VALUE ) {
			do {
				for ( const std::string &n : ZipNames( dir + "\\" + fd.cFileName ) ) {
					const std::string l = Lower( n );
					g_allFiles.push_back( l );
					if ( l.compare( 0, 5, "maps/" ) == 0 && EndsWith( l, ".bsp" ) )
						found[l] = { n, fd.cFileName };
				}
			} while ( FindNextFileA( h, &fd ) );
			FindClose( h );
		}
		FindLooseMaps( dir, "maps", found, std::string( folder ) + " folder" );
		FindLooseMaps( dir, "cubemaps", found, "" );	// only notes the files, for the rend2 check
	}
	std::vector<MapEntry> maps;
	for ( auto &kv : found ) {
		kv.second.rend2 = IsRend2( kv.second.bsp );
		maps.push_back( kv.second );
	}
	return maps;
}

// ---------------------------------------------------------------------------
// The player's settings: the baked file must be made with exactly the values the
// game will use, or the game would call it out of date and bake again. So the
// compiler gets every DevoGI setting from eternaljk.cfg (or the game's default).
// ---------------------------------------------------------------------------

struct Setting {
	const char *cvar;
	const char *flag;		// compiler option
	const char *def;		// the game's default
};

static const Setting kSettings[] = {
	{ "r_devoGI_scale",			"-scale",			"12" },
	{ "r_devoGI_rays",			"-rays",			"128" },
	{ "r_devoGI_bounces",		"-bounces",			"16" },
	{ "r_devoGI_liveBounces",	"-livebounces",		"24" },
	{ "r_devoGI_liveSky",		"-livesky",			"6" },
	{ "r_devoGI_lampBoost",		"-lampboost",		"1.5" },
	{ "r_devoGI_aa",			"-aa",				"2.0" },
	{ "r_devoGI_sheen",			"-sheen",			"0.12" },
	{ "r_devoGI_intensity",		"-intensity",		"1.0" },
	{ "r_devoGI_exposure",		"-exposure",		"1.0" },
	{ "r_devoGI_sky",			"-sky",				"0.5" },
	{ "r_devoGI_saturation",	"-saturation",		"1.6" },
	{ "r_devoGI_ao",			"-ao",				"1.6" },
	{ "r_devoGI_aoDistance",	"-aodistance",		"256" },
	{ "r_devoGI_aoDirect",		"-aodirect",		"0.15" },
	{ "r_devoLights_glow",		"-glow",			"4000" },
	{ "r_devoGI_relight",		"-relight",			"1" },
	{ "r_devoGI_entityLights",	"-entitylights",	"1" },
	{ "r_devoGI_threads",		"-threads",			"0" },
};

static std::map<std::string, std::string> ReadConfig( const std::string &path )
{
	std::map<std::string, std::string> cvars;
	FILE *f = fopen( path.c_str(), "r" );
	if ( !f )
		return cvars;
	char line[1024];
	while ( fgets( line, sizeof( line ), f ) ) {
		// seta name "value"
		char cmd[32], name[256], value[512];
		if ( sscanf( line, "%31s %255s \"%511[^\"]\"", cmd, name, value ) == 3 && ( !_stricmp( cmd, "seta" ) || !_stricmp( cmd, "set" ) ) )
			cvars[Lower( name )] = value;
	}
	fclose( f );
	return cvars;
}

static std::string CompilerSettings( const std::map<std::string, std::string> &cfg )
{
	std::string args;
	for ( const Setting &s : kSettings ) {
		auto it = cfg.find( Lower( s.cvar ) );
		args += " ";
		args += s.flag;
		args += " ";
		args += it != cfg.end() ? it->second : s.def;
	}
	auto gpu = cfg.find( "r_devogi_gpu" );
	if ( gpu != cfg.end() && atoi( gpu->second.c_str() ) == 0 )
		args += " -nogpu";
	return args;
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

enum {
	ID_LIST = 100, ID_MODE, ID_PREBAKE, ID_EXIT, ID_ALL, ID_NONE, ID_STATUS,
	WM_BAKE_ROW = WM_APP + 1,	// wParam = row, lParam = new status text (heap, freed by the UI)
	WM_BAKE_LINE,				// lParam = the compiler's latest output line (heap)
	WM_BAKE_DONE,
};

static HWND g_wnd, g_list, g_mode, g_prebake, g_exit, g_all, g_none, g_status;
static std::string g_game, g_compiler;
static std::vector<MapEntry> g_maps;
static HANDLE g_worker;
static volatile LONG g_cancel;
static HANDLE g_child;		// the compiler process that is running right now

static std::string OutputName( const MapEntry &m, bool live )
{
	std::string base = m.bsp.substr( 0, m.bsp.size() - 4 );	// without .bsp
	return g_game + "\\EternalJK\\" + base + ( live ? ".live.devogi" : ".devogi" );
}

static const char *kRend2Status = "Rend2 map, *.HDR and *.RAW detected, not recommended to bake";

static std::string StatusOf( const MapEntry &m )
{
	if ( m.rend2 )
		return kRend2Status;
	const bool high = FileExists( OutputName( m, true ) ), low = FileExists( OutputName( m, false ) );
	if ( high && low )
		return "high + low";
	if ( high )
		return "high";
	if ( low )
		return "low";
	return "-";
}

static void SetRow( int row, int column, const std::string &text )
{
	ListView_SetItemText( g_list, row, column, (LPSTR)text.c_str() );
}

static void PostText( UINT msg, WPARAM w, const std::string &text )
{
	PostMessageA( g_wnd, msg, w, (LPARAM)_strdup( text.c_str() ) );
}

// runs the compiler for one map and one mode; true when it finished well
static bool BakeOne( const MapEntry &m, bool live, const std::string &settings )
{
	const std::string out = OutputName( m, live );
	// make sure the target folder exists (maps/mp/ etc.)
	for ( size_t i = g_game.size() + 1; ( i = out.find( '\\', i ) ) != std::string::npos; i++ )
		CreateDirectoryA( out.substr( 0, i ).c_str(), NULL );

	std::string cmd = "\"" + g_compiler + "\" -game ja -fs_basepath \"" + g_game + "\" -fs_game EternalJK -devogi"
		+ ( live ? " -live" : "" ) + settings + " -out \"" + out + "\" " + m.bsp;

	// the compiler's output comes back through a pipe, so the window can show its progress
	SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
	HANDLE readEnd, writeEnd;
	if ( !CreatePipe( &readEnd, &writeEnd, &sa, 0 ) )
		return false;
	SetHandleInformation( readEnd, HANDLE_FLAG_INHERIT, 0 );

	STARTUPINFOA si = {};
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = si.hStdError = writeEnd;
	si.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
	PROCESS_INFORMATION pi = {};
	std::vector<char> cmdLine( cmd.begin(), cmd.end() );
	cmdLine.push_back( 0 );
	const BOOL started = CreateProcessA( NULL, cmdLine.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
		NULL, g_game.c_str(), &si, &pi );
	CloseHandle( writeEnd );
	if ( !started ) {
		CloseHandle( readEnd );
		return false;
	}
	g_child = pi.hProcess;

	// progress lines end in \r (they overwrite themselves in a console), others in \n
	std::string line;
	char buf[512];
	DWORD got;
	while ( ReadFile( readEnd, buf, sizeof( buf ), &got, NULL ) && got ) {
		for ( DWORD i = 0; i < got; i++ ) {
			if ( buf[i] == '\r' || buf[i] == '\n' ) {
				if ( !line.empty() )
					PostText( WM_BAKE_LINE, 0, line );
				line.clear();
			} else {
				line += buf[i];
			}
		}
	}
	CloseHandle( readEnd );
	WaitForSingleObject( pi.hProcess, INFINITE );
	DWORD code = 1;
	GetExitCodeProcess( pi.hProcess, &code );
	g_child = NULL;
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return code == 0 && !g_cancel && FileExists( out );
}

struct Job {
	int row;
	MapEntry map;
};
static std::vector<Job> g_jobs;
static int g_modeChoice;		// 0 high, 1 low, 2 both

static DWORD WINAPI Worker( LPVOID )
{
	const std::string settings = CompilerSettings( ReadConfig( g_game + "\\EternalJK\\eternaljk.cfg" ) );
	for ( const Job &job : g_jobs ) {
		if ( g_cancel )
			break;
		const bool doHigh = g_modeChoice == 0 || g_modeChoice == 2;
		const bool doLow = g_modeChoice == 1 || g_modeChoice == 2;
		bool ok = true;
		if ( doHigh && !g_cancel ) {
			PostText( WM_BAKE_ROW, job.row, "baking high spec..." );
			ok = BakeOne( job.map, true, settings ) && ok;
		}
		if ( doLow && !g_cancel ) {
			PostText( WM_BAKE_ROW, job.row, "baking low spec..." );
			ok = BakeOne( job.map, false, settings ) && ok;
		}
		PostText( WM_BAKE_ROW, job.row, g_cancel ? "stopped" : ok ? ( job.map.rend2 ? "baked, but the game keeps its rend2 lighting" : StatusOf( job.map ) ) : "FAILED - see the compiler log" );
	}
	PostMessageA( g_wnd, WM_BAKE_DONE, 0, 0 );
	return 0;
}

static void SetBusy( bool busy )
{
	EnableWindow( g_prebake, !busy );
	EnableWindow( g_mode, !busy );
	EnableWindow( g_all, !busy );
	EnableWindow( g_none, !busy );
	SetWindowTextA( g_exit, busy ? "Stop" : "Exit" );
}

static void StartPrebake()
{
	g_jobs.clear();
	const int n = ListView_GetItemCount( g_list );
	for ( int i = 0; i < n; i++ )
		if ( ListView_GetCheckState( g_list, i ) )
			g_jobs.push_back( { i, g_maps[i] } );
	if ( g_jobs.empty() ) {
		MessageBoxA( g_wnd, "Tick the maps you want to prebake first.", "Precompile Shaders", MB_OK | MB_ICONINFORMATION );
		return;
	}
	if ( !FileExists( g_compiler ) ) {
		MessageBoxA( g_wnd, "mapcompiler.exe was not found next to this program.\nPut both in the game folder.", "Precompile Shaders", MB_OK | MB_ICONERROR );
		return;
	}
	g_modeChoice = (int)SendMessageA( g_mode, CB_GETCURSEL, 0, 0 );
	g_cancel = 0;
	SetBusy( true );
	g_worker = CreateThread( NULL, 0, Worker, NULL, 0, NULL );
}

static void Layout( int w, int h )
{
	MoveWindow( g_list, 10, 10, w - 20, h - 90, TRUE );
	MoveWindow( g_status, 10, h - 72, w - 20, 20, TRUE );
	MoveWindow( g_all, 10, h - 44, 80, 30, TRUE );
	MoveWindow( g_none, 95, h - 44, 80, 30, TRUE );
	MoveWindow( g_mode, 190, h - 40, 130, 200, TRUE );
	MoveWindow( g_prebake, w - 210, h - 44, 95, 30, TRUE );
	MoveWindow( g_exit, w - 105, h - 44, 95, 30, TRUE );
}

static void FillList()
{
	ListView_DeleteAllItems( g_list );
	for ( size_t i = 0; i < g_maps.size(); i++ ) {
		LVITEMA it = {};
		it.mask = LVIF_TEXT;
		it.iItem = (int)i;
		it.pszText = (LPSTR)g_maps[i].bsp.c_str();
		ListView_InsertItem( g_list, &it );
		SetRow( (int)i, 1, g_maps[i].source );
		SetRow( (int)i, 2, StatusOf( g_maps[i] ) );
	}
	char text[256];
	snprintf( text, sizeof( text ), "%d maps found. Baked files go to %s\\EternalJK\\maps", (int)g_maps.size(), g_game.c_str() );
	SetWindowTextA( g_status, text );
}

static LRESULT CALLBACK WndProc( HWND wnd, UINT msg, WPARAM wp, LPARAM lp )
{
	switch ( msg ) {
	case WM_CREATE: {
		HFONT font = (HFONT)GetStockObject( DEFAULT_GUI_FONT );
		auto make = [&]( const char *cls, const char *text, DWORD style, int id ) {
			HWND h = CreateWindowExA( 0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, wnd, (HMENU)(INT_PTR)id, NULL, NULL );
			SendMessageA( h, WM_SETFONT, (WPARAM)font, TRUE );
			return h;
		};
		g_list = make( WC_LISTVIEWA, "", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER, ID_LIST );
		ListView_SetExtendedListViewStyle( g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES );
		const char *cols[] = { "Map", "Found in", "Baked" };
		const int widths[] = { 230, 150, 320 };
		for ( int i = 0; i < 3; i++ ) {
			LVCOLUMNA c = {};
			c.mask = LVCF_TEXT | LVCF_WIDTH;
			c.pszText = (LPSTR)cols[i];
			c.cx = widths[i];
			ListView_InsertColumn( g_list, i, &c );
		}
		g_status = make( "STATIC", "", SS_LEFTNOWORDWRAP, ID_STATUS );
		g_all = make( "BUTTON", "All", BS_PUSHBUTTON, ID_ALL );
		g_none = make( "BUTTON", "None", BS_PUSHBUTTON, ID_NONE );
		g_mode = make( "COMBOBOX", "", CBS_DROPDOWNLIST, ID_MODE );
		for ( const char *s : { "High spec", "Low spec", "Both" } )
			SendMessageA( g_mode, CB_ADDSTRING, 0, (LPARAM)s );
		SendMessageA( g_mode, CB_SETCURSEL, 2, 0 );
		g_prebake = make( "BUTTON", "Prebake", BS_DEFPUSHBUTTON, ID_PREBAKE );
		g_exit = make( "BUTTON", "Exit", BS_PUSHBUTTON, ID_EXIT );
		return 0;
	}
	case WM_SIZE:
		Layout( LOWORD( lp ), HIWORD( lp ) );
		return 0;
	case WM_GETMINMAXINFO:
		( (MINMAXINFO *)lp )->ptMinTrackSize.x = 560;
		( (MINMAXINFO *)lp )->ptMinTrackSize.y = 300;
		return 0;
	case WM_COMMAND:
		switch ( LOWORD( wp ) ) {
		case ID_PREBAKE:
			StartPrebake();
			return 0;
		case ID_ALL:
		case ID_NONE:
			// "All" leaves rend2 maps out: the game wouldn't use their DevoGI lighting
			for ( int i = 0; i < ListView_GetItemCount( g_list ); i++ )
				ListView_SetCheckState( g_list, i, LOWORD( wp ) == ID_ALL && !g_maps[i].rend2 );
			return 0;
		case ID_EXIT:
			if ( g_worker ) {	// "Stop": finish nothing more, end the bake that is running
				InterlockedExchange( &g_cancel, 1 );
				if ( g_child )
					TerminateProcess( g_child, 1 );
				return 0;
			}
			DestroyWindow( wnd );
			return 0;
		}
		break;
	case WM_BAKE_ROW:
		SetRow( (int)wp, 2, (char *)lp );
		ListView_EnsureVisible( g_list, (int)wp, FALSE );
		free( (void *)lp );
		return 0;
	case WM_BAKE_LINE:
		SetWindowTextA( g_status, (char *)lp );
		free( (void *)lp );
		return 0;
	case WM_BAKE_DONE:
		WaitForSingleObject( g_worker, INFINITE );
		CloseHandle( g_worker );
		g_worker = NULL;
		SetBusy( false );
		SetWindowTextA( g_status, g_cancel ? "Stopped." : "Done. Start the game - the maps load with their new lighting right away." );
		return 0;
	case WM_CLOSE:
		if ( g_worker ) {
			if ( MessageBoxA( wnd, "A bake is still running. Stop it and exit?", "Precompile Shaders", MB_YESNO | MB_ICONQUESTION ) != IDYES )
				return 0;
			InterlockedExchange( &g_cancel, 1 );
			if ( g_child )
				TerminateProcess( g_child, 1 );
			WaitForSingleObject( g_worker, 5000 );
		}
		DestroyWindow( wnd );
		return 0;
	case WM_DESTROY:
		PostQuitMessage( 0 );
		return 0;
	}
	return DefWindowProcA( wnd, msg, wp, lp );
}

int WINAPI WinMain( HINSTANCE inst, HINSTANCE, LPSTR, int show )
{
	INITCOMMONCONTROLSEX icc = { sizeof( icc ), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
	InitCommonControlsEx( &icc );

	// the game folder: next to this program, or given on the command line
	g_game = ExeFolder();
	if ( __argc > 1 )
		g_game = __argv[1];
	g_compiler = ExeFolder() + "\\mapcompiler.exe";
	if ( !FileExists( g_game + "\\base\\assets0.pk3" ) ) {
		MessageBoxA( NULL, "Put Precompile Shaders (and mapcompiler.exe) in the game folder,\n"
			"next to eternaljk.x86_64.exe and the base folder.", "Precompile Shaders", MB_OK | MB_ICONINFORMATION );
		return 1;
	}
	g_maps = FindMaps( g_game );

	WNDCLASSA wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor( NULL, IDC_ARROW );
	wc.hbrBackground = (HBRUSH)( COLOR_BTNFACE + 1 );
	wc.hIcon = LoadIcon( NULL, IDI_APPLICATION );
	wc.lpszClassName = "DevoPrecompile";
	RegisterClassA( &wc );
	g_wnd = CreateWindowA( wc.lpszClassName, "Precompile Shaders - DevoJK lighting", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 720, 560, NULL, NULL, inst, NULL );
	FillList();
	ShowWindow( g_wnd, show );

	MSG m;
	while ( GetMessageA( &m, NULL, 0, 0 ) > 0 ) {
		TranslateMessage( &m );
		DispatchMessageA( &m );
	}
	return 0;
}
