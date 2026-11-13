#pragma once
#include "assetsys.h"
#include <string>
#include <vector>

class AssetsLoader
{
	assetsys_t *assets;
	int _load(const assetsys_file_t &file, void **buffer) const;
public:
	AssetsLoader();
	~AssetsLoader();
	void mount(const std::string &path) const;
	void unmount(const std::string &path) const;
	int load(const std::string &filePath, void **buffer) const;
	int load(const std::string &filePath, void **buffer, int index) const;
	bool exists(const std::string &filePath) const;
	int refCount(const std::string &filePath) const;
	// DevoGI: names of the files directly in a folder with the given extension
	// (e.g. "shaders", ".shader"), from every mount, each name once
	std::vector<std::string> listFiles(const std::string &dir, const std::string &ext) const;
};
