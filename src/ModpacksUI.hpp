#pragma once
// just the popup functions, actual ui is in the cpp
#include <Geode/Geode.hpp>
#include <filesystem>

using namespace geode::prelude;

void openModpacksPopup();
void openPackFile(std::filesystem::path path);
