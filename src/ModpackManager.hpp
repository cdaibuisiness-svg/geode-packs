#pragma once
// pack saving / loading, just ids + versions, nothing else

#include <Geode/Geode.hpp>
#include <matjson.hpp>
#include <algorithm>
#include <ctime>
#include <vector>

using namespace geode::prelude;

namespace gmpkg {

struct ModEntry {
    std::string id;
    std::string version;
};

struct Pack {
    std::string name;
    std::string gameVersion;
    long long created = 0;
    std::vector<ModEntry> mods;
};

// strip stuff windows hates in filenames
inline std::string sanitizeName(std::string const& name) {
    std::string out;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == ' ' || c == '.') {
            out += c;
        }
    }
    // trim spaces
    auto first = out.find_first_not_of(' ');
    auto last = out.find_last_not_of(' ');
    if (first == std::string::npos) out.clear();
    else out = out.substr(first, last - first + 1);
    if (out.size() > 48) out = out.substr(0, 48);
    if (out.empty()) out = "modpack";
    return out;
}

inline std::filesystem::path packsDir() {
    auto dir = Mod::get()->getSaveDir() / "modpacks";
    (void)geode::utils::file::createDirectoryAll(dir);
    return dir;
}

inline std::filesystem::path pathFor(std::string const& name) {
    std::string safe = sanitizeName(name);
    for (auto& c : safe) if (c == ' ') c = '_';
    return packsDir() / (safe + ".gmpkg");
}

// json breaks if names have quotes so fix that
inline std::string escapeJson(std::string const& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

inline std::string manifestJson(Pack const& pack, int format) {
    std::string json = "{\n";
    json += "  \"format\": " + std::to_string(format) + ",\n";
    json += "  \"name\": \"" + escapeJson(pack.name) + "\",\n";
    json += "  \"gameVersion\": \"" + escapeJson(pack.gameVersion) + "\",\n";
    json += "  \"created\": " + std::to_string(pack.created) + ",\n";
    json += "  \"mods\": [\n";
    for (size_t i = 0; i < pack.mods.size(); i++) {
        json += "    {\"id\": \"" + escapeJson(pack.mods[i].id) +
                "\", \"version\": \"" + escapeJson(pack.mods[i].version) + "\"}";
        if (i + 1 < pack.mods.size()) json += ",";
        json += "\n";
    }
    json += "  ]\n}\n";
    return json;
}

inline geode::Result<Pack> packFromJson(matjson::Value const& json, std::string const& fallbackName) {
    Pack pack;
    pack.name = json["name"].asString().unwrapOr(fallbackName);
    pack.gameVersion = json["gameVersion"].asString().unwrapOr("?");
    auto createdRes = json["created"].asInt();
    pack.created = createdRes.isOk() ? (long long)createdRes.unwrap() : 0;
    auto modsVal = json["mods"];
    if (!modsVal.isArray()) {
        return geode::Err("Invalid .gmpkg file (missing mods list).");
    }
    for (size_t i = 0; i < modsVal.size(); i++) {
        auto& m = modsVal[i];
        ModEntry e;
        e.id = m["id"].asString().unwrapOr("");
        e.version = m["version"].asString().unwrapOr("");
        if (!e.id.empty()) pack.mods.push_back(std::move(e));
    }
    return geode::Ok(std::move(pack));
}

// old packs were zips with bundled files, still support reading them
inline bool isZipFile(std::filesystem::path const& path) {
    auto data = geode::utils::file::readBinary(path);
    if (data.isErr()) return false;
    auto const& bytes = data.unwrap();
    return bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' &&
        bytes[2] == '\x03' && bytes[3] == '\x04';
}

inline geode::Result<Pack> readManifest(std::filesystem::path const& path) {
    std::string fallback = path.stem().string();
    if (isZipFile(path)) {
        auto unzip = geode::utils::file::Unzip::create(path);
        if (unzip.isErr()) return geode::Err("Could not open .gmpkg (not a valid pack).");
        if (!unzip.unwrap().hasEntry("modpack.json")) {
            return geode::Err("Invalid .gmpkg file (no manifest).");
        }
        auto raw = unzip.unwrap().extract("modpack.json");
        if (raw.isErr()) return geode::Err("Could not read pack manifest.");
        auto const& bytes = raw.unwrap();
        std::string text(bytes.begin(), bytes.end());
        auto parsed = matjson::Value::parse(text);
        if (parsed.isErr()) return geode::Err("Could not parse pack manifest.");
        return packFromJson(parsed.unwrap(), fallback);
    }
    GEODE_UNWRAP_INTO(auto text, geode::utils::file::readString(path));
    auto parsed = matjson::Value::parse(text);
    if (parsed.isErr()) {
        return geode::Err("Could not parse .gmpkg file (invalid JSON).");
    }
    return packFromJson(parsed.unwrap(), fallback);
}

inline geode::Result<Pack> loadPack(std::filesystem::path const& path) {
    return readManifest(path);
}

// just write the json, way simpler than zipping everything
inline geode::Result<void> savePack(Pack const& pack) {
    auto path = pathFor(pack.name);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto res = geode::utils::file::writeString(path, manifestJson(pack, 2));
    if (res.isErr()) return geode::Err("Could not write pack file.");
    return geode::Ok();
}

struct PackInfo {
    std::string name;
    std::filesystem::path path;
    size_t modCount = 0;
};

inline std::vector<PackInfo> listPacks() {
    std::vector<PackInfo> out;
    auto dir = packsDir();
    auto files = geode::utils::file::readDirectory(dir, false);
    if (files.isErr()) return out;
    for (auto& p : files.unwrap()) {
        if (p.extension() != ".gmpkg") continue;
        PackInfo info;
        info.path = p;
        info.name = p.stem().string();
        info.modCount = 0;
        if (auto res = readManifest(p); res.isOk()) {
            auto pack = res.unwrap();
            info.name = pack.name;
            info.modCount = pack.mods.size();
        }
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) {
        return a.name < b.name;
    });
    return out;
}

}
