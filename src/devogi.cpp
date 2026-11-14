/* -------------------------------------------------------------------------------

   DevoGI stage ("-devogi") - Devoted

   Bakes the lighting of DevoJK's renderer ahead of time, on the CPU, from the
   command line. It takes a finished BSP (it does not change it) and writes the
   same maps/<name>.devogi file the game would write on the map's first load:

       q3map2 -game ja -fs_basepath "C:/Games/EternalJK" -fs_game EternalJK
              -devogi -scale 8 maps/mp/ffa3.bsp

   The bake itself is devogi_core.cpp, the very same file the renderer uses
   (copied from DevoJK's codemp/rd-vulkan). This file is only its "host" here:
   it reads the BSP, finds images and shader scripts through q3map2's virtual
   file system the way the game finds them, prints progress and saves the
   result. When the options below match the player's r_devoGI_* settings, the
   game finds the file, prints "using baked lighting" and skips its own bake.

   Part of DevoQ3Map2 by Devoted, licensed under GPLv2 like the rest of q3map2.

   ------------------------------------------------------------------------------- */



/* dependencies */
#include "q3map2.h"
#include "devogi_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>



/*
   Console output and the clock.
   A progress line is redrawn in place with '\r'; anything else printed while
   it is on screen first moves to a fresh line so nothing gets overwritten.
 */

static bool devogi_progressLine;

static void DevoGI_Print( const char *text ){
	// the core also prints "DevoGI: baking <map>: <step> 40%" every 10% for the game's
	// log; here the live line below already shows that, so those lines are skipped
	const size_t len = strlen( text );
	if ( !strncmp( text, "DevoGI: baking ", 15 ) && len >= 2 && text[ len - 2 ] == '%' ) {
		return;
	}
	if ( devogi_progressLine ) {
		printf( "\n" );
		devogi_progressLine = false;
	}
	Sys_Printf( "%s", text );
}

// milliseconds since the stage started (I_FloatTime only counts whole seconds)
static int DevoGI_Milliseconds( void ){
	static const auto start = std::chrono::steady_clock::now();
	return (int)std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start ).count();
}

static void DevoGI_Progress( const char *mapName, const char *stage, float overall, bool force ){
	static int lastPct = -1;
	const int pct = (int)( overall * 100.0f );
	if ( pct == lastPct && !force ) {
		return;
	}
	lastPct = pct;
	printf( "\r  %-40.40s %3d%%", stage, pct );
	fflush( stdout );
	devogi_progressLine = true;
}



/*
   Images: same name, same file, same extension order (.jpg, .png, .tga) as the game.
 */

static bool DevoGI_LoadImage( const char *name, uint8_t **rgba, int *width, int *height ){
	return DevoGI_LoadImagePixels( name, rgba, width, height );
}

static void DevoGI_FreeImage( uint8_t *rgba ){
	free( rgba );
}



/*
   Shader scripts, picked exactly like DevoJK's renderer picks them
   (ScanAndLoadShaderFiles, COM_CompressShader and FindShaderInShaderText in
   codemp/rd-vulkan/tr_shader.cpp):

   - every shaders/*.shader file, sorted by name; when a .mtr file of the same
     name exists (PBR materials) it is read instead,
   - a file with a broken shader (missing { or }) is left out completely,
   - the files are joined in reverse order, so the alphabetically LAST file
     comes first, and comments are stripped,
   - and the first definition of a name in that text wins.

   So when two files define the same shader, the one in the file whose name sorts
   last is used, in the game and here. That matters: the shader text is part of
   the cache key, so both must read exactly the same text. Each shader is found
   once and kept in a table, because the bake asks for them many times.
 */

static std::string devogi_shaderText;								// all scripts, joined
static std::unordered_map<std::string, size_t> devogi_shaderIndex;	// lower-case name -> text right after the name

static std::string DevoGI_Lower( const char *s ){
	std::string out( s );
	for ( char &c : out )
		c = (char)tolower( (unsigned char)c );
	return out;
}

// skips a { ... } block; depth = how many braces are already open (SkipBracedSection)
static bool DevoGI_SkipBraced( const char **p, int depth ){
	do {
		const char *token = DG_ParseToken( p, true );
		if ( token[ 1 ] == 0 ) {
			if ( token[ 0 ] == '{' ) {
				depth++;
			}
			else if ( token[ 0 ] == '}' ) {
				depth--;
			}
		}
	} while ( depth && *p );
	return depth == 0;
}

// skips to the start of the next line (SkipRestOfLine)
static void DevoGI_SkipRestOfLine( const char **p ){
	const char *s = *p;
	if ( !s || !*s ) {
		return;
	}
	int c;
	while ( ( c = *s++ ) != 0 ) {
		if ( c == '\n' ) {
			break;
		}
	}
	*p = s;
}

// the game's check that one bad file can't break all the others
static bool DevoGI_ShaderFileIsValid( const char *text ){
	const char *p = text;
	while ( 1 )
	{
		const char *token = DG_ParseToken( &p, true );
		if ( !token[ 0 ] ) {
			return true;
		}
		if ( token[ 0 ] == '#' ) {
			DevoGI_SkipRestOfLine( &p );
			continue;
		}
		token = DG_ParseToken( &p, true );
		if ( token[ 0 ] != '{' || token[ 1 ] != 0 ) {
			return false;	// missing opening brace
		}
		if ( !DevoGI_SkipBraced( &p, 1 ) ) {
			return false;	// missing closing brace
		}
	}
}

// strips comments and extra spaces like the game does (COM_CompressShader)
static void DevoGI_CompressShader( std::string &text ){
	char *in = &text[ 0 ], *out = in;
	bool newline = false, whitespace = false;
	int c;
	while ( ( c = *in ) != 0 )
	{
		if ( c == '/' && in[ 1 ] == '/' ) {
			while ( *in && *in != '\n' ) in++;
		}
		else if ( c == '#' ) {
			while ( *in && *in != '\n' ) in++;
		}
		else if ( c == '/' && in[ 1 ] == '*' ) {
			while ( *in && ( *in != '*' || in[ 1 ] != '/' ) ) in++;
			if ( *in ) in += 2;
		}
		else if ( c == '\n' || c == '\r' ) {
			newline = true;
			in++;
		}
		else if ( c == ' ' || c == '\t' ) {
			whitespace = true;
			in++;
		}
		else
		{
			if ( newline ) {
				*out++ = '\n';
				newline = false;
				whitespace = false;
			}
			if ( whitespace ) {
				*out++ = ' ';
				whitespace = false;
			}
			if ( c == '"' ) {	// quoted strings are copied as they are
				*out++ = (char)c;
				in++;
				while ( ( c = *in ) != 0 && c != '"' ) {
					*out++ = (char)c;
					in++;
				}
				if ( c == '"' ) {
					*out++ = (char)c;
					in++;
				}
			}
			else
			{
				*out++ = (char)c;
				in++;
			}
		}
	}
	text.resize( out - &text[ 0 ] );
}

static void DevoGI_LoadShaderScripts( void ){
	std::vector<std::string> files = g_vfs.listFiles( "shaders", ".shader" );
	std::sort( files.begin(), files.end(), []( const std::string &a, const std::string &b ) {
		return DevoGI_Lower( a.c_str() ) < DevoGI_Lower( b.c_str() );
	} );

	// read every file (or its .mtr twin), leaving out broken ones
	std::vector<std::string> texts( files.size() );
	int numUsed = 0;
	for ( size_t i = 0; i < files.size(); i++ )
	{
		const std::string base = files[ i ].substr( 0, files[ i ].size() - 7 );	// without ".shader"
		std::string name = "shaders/" + base + ".mtr";
		if ( !g_vfs.exists( name ) ) {
			name = "shaders/" + files[ i ];
		}
		char *buffer = NULL;
		const int size = g_vfs.load( name, (void**) &buffer );
		if ( size > 0 && buffer ) {
			texts[ i ].assign( buffer, strnlen( buffer, size ) );
			if ( !DevoGI_ShaderFileIsValid( texts[ i ].c_str() ) ) {
				Sys_FPrintf( SYS_WRN, "WARNING: DevoGI: %s has a broken shader, the game ignores it too\n", name.c_str() );
				texts[ i ].clear();
			}
			else {
				numUsed++;
			}
		}
		delete[] buffer;
	}

	// joined in reverse order, then compressed, like the game
	devogi_shaderText.clear();
	for ( int i = (int)files.size() - 1; i >= 0; i-- )
	{
		if ( texts[ i ].empty() ) {
			continue;
		}
		devogi_shaderText += texts[ i ];
		devogi_shaderText += "\n";
	}
	DevoGI_CompressShader( devogi_shaderText );

	// the table: name -> its text (the first definition wins)
	devogi_shaderIndex.clear();
	const char *base = devogi_shaderText.c_str();
	const char *p = base;
	while ( p && *p )
	{
		const char *token = DG_ParseToken( &p, true );
		if ( !token[ 0 ] ) {
			break;
		}
		if ( token[ 0 ] == '#' ) {
			DevoGI_SkipRestOfLine( &p );
			continue;
		}
		const std::string name = DevoGI_Lower( token );
		if ( p && devogi_shaderIndex.find( name ) == devogi_shaderIndex.end() ) {
			devogi_shaderIndex[ name ] = (size_t)( p - base );
		}
		if ( !p ) {
			break;
		}
		DevoGI_SkipBraced( &p, 0 );
	}

	Sys_Printf( "DevoGI: %d shader files, %d shaders\n", numUsed, (int)devogi_shaderIndex.size() );
}

static const char *DevoGI_ShaderText( const char *shaderName ){
	const auto it = devogi_shaderIndex.find( DevoGI_Lower( shaderName ) );
	if ( it == devogi_shaderIndex.end() ) {
		return NULL;
	}
	return devogi_shaderText.c_str() + it->second;
}



/*
   Reading the BSP: a file on disk, or a map inside a pk3 (maps/<name>.bsp).
 */

static bool DevoGI_ReadBSP( const char *path, std::vector<uint8_t> &out, std::string &vfsName ){
	FILE *f = fopen( path, "rb" );
	if ( f ) {
		fseek( f, 0, SEEK_END );
		const long len = ftell( f );
		fseek( f, 0, SEEK_SET );
		out.resize( len > 0 ? (size_t)len : 0 );
		const size_t got = len > 0 ? fread( out.data(), 1, out.size(), f ) : 0;
		fclose( f );
		vfsName.clear();
		return got == out.size() && !out.empty();
	}

	// not on disk: try the game's files ("maps/mp/ffa3" or a path ending in it)
	std::string rel = path;
	std::replace( rel.begin(), rel.end(), '\\', '/' );
	const size_t maps = DevoGI_Lower( rel.c_str() ).rfind( "maps/" );
	if ( maps != std::string::npos ) {
		rel = rel.substr( maps );
	}
	char *buffer = NULL;
	const int size = g_vfs.load( rel, (void**) &buffer );
	if ( size <= 0 || !buffer ) {
		delete[] buffer;
		return false;
	}
	out.assign( (uint8_t*) buffer, (uint8_t*) buffer + size );
	delete[] buffer;
	vfsName = rel;
	return true;
}



// creates the folders on the way to a file (e.g. maps/mp/ for maps/mp/ffa3.devogi)
static void DevoGI_MakeFolders( const char *filePath ){
	std::string p = filePath;
	std::replace( p.begin(), p.end(), '\\', '/' );
	for ( size_t k = 1; k < p.size(); k++ )
	{
		if ( p[ k ] == '/' && p[ k - 1 ] != ':' ) {
			Q_mkdir( p.substr( 0, k ).c_str() );
		}
	}
}



/*
   DevoGIMain()
   the -devogi stage
 */

int DevoGIMain( int argc, char **argv ){
	if ( argc < 2 ) {
		Sys_Printf( "Usage: q3map2 [general options] -devogi [-scale 8] [options] <mapname.bsp>\n" );
		Sys_Printf( "See q3map2 -help devogi\n" );
		return 0;
	}

	/* options: the names and defaults of the game's r_devoGI_* settings */
	dgSettings settings;
	char outPath[ 1024 ] = "";
	int maxScale = 16;	// the game also limits this by its GPU's texture size (128 * scale per page)

	// main() passes every argument; ours are the ones between "-devogi" and the map
	int i = 1;
	for ( int k = 0; k < argc - 1; k++ )
	{
		if ( !Q_stricmp( argv[ k ], "-devogi" ) ) {
			i = k + 1;
			break;
		}
	}
	for ( ; i < ( argc - 1 ); i++ )
	{
		const char *a = argv[ i ];
		auto next = [&]( void ) -> const char * {
			if ( i + 1 >= argc - 1 ) {
				Error( "DevoGI: %s needs a value", a );
			}
			return argv[ ++i ];
		};
		if ( !Q_stricmp( a, "-scale" ) ) settings.scale = atoi( next() );
		else if ( !Q_stricmp( a, "-rays" ) ) settings.rays = atoi( next() );
		else if ( !Q_stricmp( a, "-bounces" ) ) settings.bounces = atoi( next() );
		else if ( !Q_stricmp( a, "-livebounces" ) ) settings.liveBounces = atoi( next() );
		else if ( !Q_stricmp( a, "-livesky" ) ) settings.liveSky = atof( next() );
		else if ( !Q_stricmp( a, "-aa" ) ) settings.aa = atof( next() );
		else if ( !Q_stricmp( a, "-sheen" ) ) settings.sheen = atof( next() );
		else if ( !Q_stricmp( a, "-intensity" ) ) settings.intensity = atof( next() );
		else if ( !Q_stricmp( a, "-exposure" ) ) settings.exposure = atof( next() );
		else if ( !Q_stricmp( a, "-sky" ) ) settings.sky = atof( next() );
		else if ( !Q_stricmp( a, "-saturation" ) ) settings.saturation = atof( next() );
		else if ( !Q_stricmp( a, "-ao" ) ) settings.ao = atof( next() );
		else if ( !Q_stricmp( a, "-aodistance" ) ) settings.aoDistance = atof( next() );
		else if ( !Q_stricmp( a, "-aodirect" ) ) settings.aoDirect = atof( next() );
		else if ( !Q_stricmp( a, "-glow" ) ) settings.glow = atof( next() );
		else if ( !Q_stricmp( a, "-relight" ) ) settings.relight = atoi( next() );
		else if ( !Q_stricmp( a, "-norelight" ) ) settings.relight = 0;
		else if ( !Q_stricmp( a, "-entitylights" ) ) settings.entityLights = atoi( next() );
		else if ( !Q_stricmp( a, "-threads" ) ) settings.threads = atoi( next() );
		else if ( !Q_stricmp( a, "-nogpu" ) ) settings.gpu = 0;
		else if ( !Q_stricmp( a, "-maxscale" ) ) maxScale = atoi( next() );
		else if ( !Q_stricmp( a, "-out" ) ) strcpy( outPath, ExpandArg( next() ) );
		else Sys_FPrintf( SYS_WRN, "WARNING: DevoGI: unknown option \"%s\"\n", a );
	}

	/* the map */
	char source[ 1024 ];
	strcpy( source, ExpandArg( argv[ i ] ) );
	StripExtension( source );
	DefaultExtension( source, ".bsp" );

	std::vector<uint8_t> bsp;
	std::string vfsName;
	if ( !DevoGI_ReadBSP( source, bsp, vfsName ) && !DevoGI_ReadBSP( argv[ i ], bsp, vfsName ) ) {
		Error( "DevoGI: can't find %s (on disk or in the game's pk3 files)", source );
	}
	if ( bsp.size() < 8 + 18 * 8 || memcmp( bsp.data(), "RBSP", 4 ) ) {
		Error( "DevoGI: %s is not a Jedi Academy (RBSP) map", source );
	}

	/* the name the game knows the map by: maps/.../<name>.bsp */
	std::string mapName = vfsName.empty() ? std::string( source ) : vfsName;
	std::replace( mapName.begin(), mapName.end(), '\\', '/' );
	{
		const size_t maps = DevoGI_Lower( mapName.c_str() ).rfind( "maps/" );
		if ( maps != std::string::npos ) {
			mapName = mapName.substr( maps );
		}
	}

	/* where the result goes: next to the BSP, or maps/... under the current folder */
	if ( !outPath[ 0 ] ) {
		std::string o = vfsName.empty() ? std::string( source ) : mapName;
		o = o.substr( 0, o.size() - 4 ) + ".devogi";
		strcpy( outPath, o.c_str() );
	}

	Sys_Printf( "--- DevoGI ---\n" );
	Sys_Printf( "Map:      %s%s\n", mapName.c_str(), vfsName.empty() ? "" : " (from the game's pk3 files)" );
	Sys_Printf( "Settings: scale %d, rays %d, bounces %d, aa %g, sheen %g, exposure %g, glow %g, relight %d\n",
				settings.scale, settings.rays, settings.bounces, settings.aa, settings.sheen, settings.exposure, settings.glow, settings.relight );

	static const dgHost host = {
		DevoGI_Print, DevoGI_Milliseconds, DevoGI_LoadImage, DevoGI_FreeImage, DevoGI_ShaderText, DevoGI_Progress
	};
	DG_SetHost( &host );
	DevoGI_LoadShaderScripts();

	/* the same steps the game takes on a map's first load */
	DG_ExtractLights( mapName.c_str(), bsp.data(), settings );
	const int scale = DG_ChooseScale( settings, maxScale );
	if ( scale != settings.scale ) {
		Sys_Printf( "DevoGI: scale %d is used (1 to 16, limited by -maxscale)\n", scale );
	}

	dgResult result;
	if ( !DG_Bake( mapName.c_str(), bsp.data(), settings, scale, result ) ) {
		Error( "DevoGI: %s has no lightmaps inside the BSP (external lightmaps are not supported yet)", mapName.c_str() );
	}

	const std::vector<uint8_t> file = DG_WriteCache( DG_BspHash( bsp.data() ), DG_SettingsHash( settings, scale ), result );
	DevoGI_MakeFolders( outPath );
	FILE *f = fopen( outPath, "wb" );
	if ( !f || fwrite( file.data(), 1, file.size(), f ) != file.size() ) {
		if ( f ) {
			fclose( f );
		}
		Error( "DevoGI: can't write %s", outPath );
	}
	fclose( f );

	if ( devogi_progressLine ) {
		printf( "\n" );
		devogi_progressLine = false;
	}
	Sys_Printf( "DevoGI: wrote %s (%.1f MB, %dx%d lightmaps)\n", outPath, file.size() / ( 1024.0 * 1024.0 ), result.pageSize, result.pageSize );
	Sys_Printf( "Put it in the game's maps folder (e.g. EternalJK/%s.devogi). The game uses it when\n",
				mapName.substr( 0, mapName.size() - 4 ).c_str() );
	Sys_Printf( "the player's r_devoGI_* settings match the options used here (r_devoGI_scale %d).\n", scale );
	return 0;
}
