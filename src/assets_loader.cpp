#define ASSETSYS_IMPLEMENTATION
#define STRPOOL_IMPLEMENTATION
#include "assetsys.h"
#include "assets_loader.hpp"
#include <cctype>

void replaceAll(std::string& str, const std::string& from, const std::string& to) {
	if (from.empty())
		return;
	size_t start_pos = 0;
	while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
		str.replace(start_pos, from.length(), to);
		start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
	}
}

AssetsLoader::AssetsLoader()
{
	assets = assetsys_create(nullptr);
}

AssetsLoader::~AssetsLoader()
{
	assetsys_destroy(assets);
}

void AssetsLoader::mount(const std::string &path) const
{
	assetsys_mount(assets, path.c_str(), "/");
}

void AssetsLoader::unmount(const std::string &path) const
{
	assetsys_dismount(assets, path.c_str(), "/");
}

int AssetsLoader::load(const std::string &filePath, void **buffer) const
{
	assetsys_file_t file;
	std::string copy = "//" + filePath;
	replaceAll(copy, "\\", "/");
	auto error = assetsys_file(assets, copy.c_str(), &file);

	if (error == ASSETSYS_ERROR_FILE_NOT_FOUND)
	{
		*buffer = nullptr;
		return -1;
	}

	return _load(file, buffer);
}

int AssetsLoader::load(const std::string& filePath, void** buffer, int index) const
{
	assetsys_file_t file;
	auto error = assetsys_file_by_index(assets, ("//" + filePath).c_str(), &file, index);

	if (error == ASSETSYS_ERROR_FILE_NOT_FOUND)
	{
		*buffer = nullptr;
		return -1;
	}

	return _load(file, buffer);
}

int AssetsLoader::_load(const assetsys_file_t &file, void **buffer) const
{
	int fileSize = assetsys_file_size(assets, file);
	*buffer = new char[fileSize + 1];

	int readSize = 0;
	assetsys_file_load(assets, file, &readSize, *buffer, fileSize);
	static_cast<char*>(*buffer)[fileSize] = '\0';

	return readSize;
}

bool AssetsLoader::exists(const std::string& filePath) const
{
	assetsys_file_t file;
	auto error = assetsys_file(assets, ("//" + filePath).c_str(), &file);

	return error == ASSETSYS_SUCCESS;
}

int AssetsLoader::refCount(const std::string& filePath) const
{
	return assetsys_file_ref_count(assets, ("//" + filePath).c_str());
}

// case-insensitive compare that works on every platform
static int devogi_stricmp(const char *a, const char *b)
{
	for (;; a++, b++)
	{
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb || !ca)
			return ca - cb;
	}
}

// DevoGI: lists the files in one folder of the virtual file system, across every
// mounted folder and pk3, so the -devogi stage can read all shader scripts the way
// the game does. Names come back without the folder, each one once (case-insensitive).
std::vector<std::string> AssetsLoader::listFiles(const std::string &dir, const std::string &ext) const
{
	std::vector<std::string> names;
	// files are stored under "//" (mounted as "/", like load() above); "/" as a fallback
	std::string path = "//" + dir;
	if (assetsys_file_count(assets, path.c_str()) == 0)
		path = "/" + dir;
	const int count = assetsys_file_count(assets, path.c_str());
	for (int i = 0; i < count; i++)
	{
		const char *name = assetsys_file_name(assets, path.c_str(), i);
		if (!name)
			continue;
		std::string n = name;
		if (n.size() < ext.size() || devogi_stricmp(n.c_str() + n.size() - ext.size(), ext.c_str()) != 0)
			continue;
		bool seen = false;
		for (const std::string &o : names)
			if (devogi_stricmp(o.c_str(), n.c_str()) == 0) { seen = true; break; }
		if (!seen)
			names.push_back(n);
	}
	return names;
}
