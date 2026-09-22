#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <switch.h>
#include <curl/curl.h>
#include <fstream>
#include <vector>
#include <string>
#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "json.hpp"
using json = nlohmann::json;

// --- SHARED STATE & GLOBALS ---
std::atomic<int> g_progress(0);
std::atomic<bool> g_is_downloading(false);
std::atomic<bool> g_is_extracting(false);
std::atomic<bool> g_is_finished(false);
std::string g_target_version_tag = "";

struct AssetInfo {
    std::string url;
    std::string filename;
};

std::vector<AssetInfo> g_target_assets;

// App Navigation States
enum AppScreen {
    SCREEN_MAIN_MENU,
    SCREEN_ATMOSPHERE_MENU,
    SCREEN_HEKATE_MENU,
    SCREEN_HOMEBREW_MENU,
    SCREEN_UPDATING
};

struct ReleaseItem {
    std::string name;
    std::string tag;
    std::string url;
};

struct HomebrewItem {
    std::string name;
    std::string api_url;
    std::string default_filename;
    std::string keyword;
};

// Global data structures
std::vector<ReleaseItem> g_atmosphere_releases;
std::string g_local_atmosphere_version = "Checking...";
bool g_atmosphere_loaded = false;

// Bypass standard Linux file permissions for the Switch OS
extern "C" mode_t umask(mode_t mask) {
    return 0;
}

// Callback for writing downloaded data
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// Progress callback
int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    if (dltotal > 0) {
        g_progress = (int)(((double)dlnow / (double)dltotal) * 100.0);
    }
    return 0; 
}

// Helper function to download any URL
bool download_file(const char* url, const char* output_path) {
    bool success = false;
    CURL *curl = curl_easy_init();
    
    if(curl) {
        FILE *fp = fopen(output_path, "wb");
        if(fp) {
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Abyss-Updater");
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); 
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            
            CURLcode res = curl_easy_perform(curl);
            if(res == CURLE_OK) success = true;
            fclose(fp);
        }
        curl_easy_cleanup(curl);
    }
    return success;
}

// Helper function to extract a zip file
bool extract_zip(const char *filename) {
    struct archive *a;
    struct archive *ext;
    struct archive_entry *entry;
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT;
    int r;

    a = archive_read_new();
    archive_read_support_format_zip(a);
    ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, flags);

    if ((r = archive_read_open_filename(a, filename, 10240))) return false;

    chdir("sdmc:/");
    int file_count = 0;
    
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (file_count % 15 == 0) g_progress = (g_progress + 2) % 100; 
        file_count++;

        r = archive_write_header(ext, entry);
        if (r >= ARCHIVE_OK && archive_entry_size(entry) > 0) {
            const void *buff;
            size_t size;
            la_int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                archive_write_data_block(ext, buff, size, offset);
            }
        }
        archive_write_finish_entry(ext);
    }
    
    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    return true;
}

// Check if a specific homebrew app, overlay, or patch is installed on the SD card
bool is_app_installed(const std::string& keyword) {
    std::string lower_kw = keyword;
    for (auto &c : lower_kw) c = tolower(c);

    if (lower_kw == "patches") {
        struct stat st;
        return (stat("sdmc:/atmosphere/exefs_patches", &st) == 0 || stat("sdmc:/atmosphere/contents", &st) == 0);
    }

    // Check sdmc:/switch/
    DIR *dir = opendir("sdmc:/switch");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            std::string name = ent->d_name;
            for (auto &c : name) c = tolower(c);
            if (name.find(lower_kw) != std::string::npos) {
                closedir(dir);
                return true;
            }
        }
        closedir(dir);
    }

    // Check sdmc:/switch/.overlays/
    dir = opendir("sdmc:/switch/.overlays");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            std::string name = ent->d_name;
            for (auto &c : name) c = tolower(c);
            if (name.find(lower_kw) != std::string::npos) {
                closedir(dir);
                return true;
            }
        }
        closedir(dir);
    }

    return false;
}

// Helper to inspect GitHub release JSON and return matching compiled assets (ignoring source and .elf files)
std::vector<AssetInfo> get_all_release_assets(const std::string& api_url, const std::string& temp_json_path) {
    std::vector<AssetInfo> assets_list;
    bool is_hekate = (api_url.find("CTCaer/hekate") != std::string::npos);

    if (download_file(api_url.c_str(), temp_json_path.c_str())) {
        std::ifstream file(temp_json_path);
        if (file.is_open()) {
            try {
                json rel = json::parse(file);
                for (auto& asset : rel["assets"]) {
                    std::string asset_name = asset["name"];
                    
                    std::string lower_name = asset_name;
                    for (auto &c : lower_name) c = tolower(c);
                    
                    if (lower_name.find("source") != std::string::npos || lower_name.find(".elf") != std::string::npos) {
                        continue; 
                    }

                    if (is_hekate) {
                        if (lower_name.find(".zip") != std::string::npos && lower_name.find("nyx") != std::string::npos) {
                            assets_list.push_back({asset["browser_download_url"], asset_name});
                            break; 
                        }
                    } else {
                        if (lower_name.find(".zip") != std::string::npos || 
                            lower_name.find(".nro") != std::string::npos || 
                            lower_name.find(".ovl") != std::string::npos) {
                            assets_list.push_back({asset["browser_download_url"], asset_name});
                        }
                    }
                }
            } catch (...) {}
            file.close();
        }
        remove(temp_json_path.c_str());
    }
    return assets_list;
}

// Check local atmosphere installation status robustly
void check_local_atmosphere() {
    std::ifstream vfile("sdmc:/atmosphere/abyss_version.txt");
    if (vfile.is_open()) {
        std::string ver;
        std::getline(vfile, ver);
        vfile.close();
        if (!ver.empty()) {
            g_local_atmosphere_version = ver;
            return;
        }
    }

    std::ifstream pfile("sdmc:/atmosphere/package.ini");
    if (pfile.is_open()) {
        std::string line;
        while (std::getline(pfile, line)) {
            if (line.find("version") != std::string::npos) {
                g_local_atmosphere_version = line;
                pfile.close();
                return;
            }
        }
        pfile.close();
    }

    u64 ams_val = 0;
    if (R_SUCCEEDED(splInitialize())) {
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &ams_val)) && ams_val != 0) {
            u32 major = (ams_val >> 24) & 0xFF;
            u32 minor = (ams_val >> 16) & 0xFF;
            u32 patch = (ams_val >> 8) & 0xFF;
            
            if (major > 0 && major < 10) {
                char ver_buf[32];
                snprintf(ver_buf, sizeof(ver_buf), "v%u.%u.%u", major, minor, patch);
                g_local_atmosphere_version = ver_buf;
                splExit();
                return;
            }
        }
        splExit();
    }

    struct stat st;
    if (stat("sdmc:/atmosphere/package3", &st) == 0) {
        g_local_atmosphere_version = "Installed (Untracked)";
    } else if (stat("sdmc:/atmosphere", &st) == 0) {
        g_local_atmosphere_version = "Installed (Folder Present)";
    } else {
        g_local_atmosphere_version = "Not Installed";
    }
}

// Synchronously load local version and fetch last 5 GitHub releases for Atmosphere
void load_atmosphere_data() {
    check_local_atmosphere();
    g_atmosphere_releases.clear();
    
    if (download_file("https://api.github.com/repos/Atmosphere-NX/Atmosphere/releases", "sdmc:/api_releases.json")) {
        std::ifstream file("sdmc:/api_releases.json");
        if (file.is_open()) {
            try {
                json releases = json::parse(file);
                int count = 0;
                for (auto& rel : releases) {
                    if (count >= 5) break; 
                    
                    std::string tag = rel.value("tag_name", "Unknown");
                    std::string name = rel.value("name", tag);
                    
                    for (auto& asset : rel["assets"]) {
                        std::string asset_name = asset["name"];
                        std::string lower_name = asset_name;
                        for (auto &c : lower_name) c = tolower(c);
                        
                        if (lower_name.find("source") != std::string::npos || lower_name.find(".elf") != std::string::npos) continue;

                        if (asset_name.find(".zip") != std::string::npos) {
                            g_atmosphere_releases.push_back({name, tag, asset["browser_download_url"]});
                            count++;
                            break;
                        }
                    }
                }
            } catch (...) {}
            file.close();
        }
        remove("sdmc:/api_releases.json");
    }
    
    if (g_atmosphere_releases.empty()) {
        g_atmosphere_releases.push_back({"Failed to fetch releases", "Error", ""});
    }
    g_atmosphere_loaded = true;
}

// Background worker thread supporting multiple file downloads (.nro, .ovl, and .zip archives)
void worker_thread() {
    g_is_downloading = true;
    g_is_extracting = false;
    g_progress = 0;
    
    int index = 0;
    for (const auto& asset : g_target_assets) {
        index++;
        if (asset.filename.find(".nro") != std::string::npos) {
            std::string dest_path = "sdmc:/switch/" + asset.filename;
            download_file(asset.url.c_str(), dest_path.c_str());
        } else if (asset.filename.find(".ovl") != std::string::npos) {
            mkdir("sdmc:/switch/.overlays", 0777);
            std::string dest_path = "sdmc:/switch/.overlays/" + asset.filename;
            download_file(asset.url.c_str(), dest_path.c_str());
        } else if (asset.filename.find(".zip") != std::string::npos) {
            std::string temp_zip = "sdmc:/update_pkg_" + std::to_string(index) + ".zip";
            if (download_file(asset.url.c_str(), temp_zip.c_str())) {
                g_is_downloading = false;
                g_is_extracting = true;
                g_progress = 0;
                
                if (extract_zip(temp_zip.c_str())) {
                    remove(temp_zip.c_str());
                    
                    if (asset.filename == "atmosphere.zip" && !g_target_version_tag.empty()) {
                        std::ofstream vfile("sdmc:/atmosphere/abyss_version.txt");
                        if (vfile.is_open()) {
                            vfile << g_target_version_tag;
                            vfile.close();
                        }
                        g_local_atmosphere_version = g_target_version_tag;
                    }
                }
                g_is_extracting = false;
                g_is_downloading = true;
            }
        }
    }

    g_is_downloading = false;
    g_is_extracting = false;
    g_is_finished = true;
    g_progress = 100;
}

int main(int argc, char **argv) {
    socketInitializeDefault(); 
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    romfsInit();
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();

    SDL_Window* window = SDL_CreateWindow("Abyss Updater", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

    TTF_Font* font = TTF_OpenFont("romfs:/font.ttf", 26);
    TTF_Font* title_font = TTF_OpenFont("romfs:/font.ttf", 44); 

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    // Menu Navigation variables
    AppScreen current_screen = SCREEN_MAIN_MENU;
    int main_selected = 0;
    int sub_selected = 0;
    
    std::vector<std::string> main_menu = {"Atmosphere", "Hekate", "Homebrew"};
    std::thread worker; 

    SDL_Color color_white = {255, 255, 255, 255};
    SDL_Color color_gray = {200, 200, 200, 255};
    SDL_Color color_green = {0, 255, 150, 255};
    SDL_Color color_dark_gray = {120, 120, 120, 255};

    bool running = true;
    
    while (appletMainLoop() && running) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        
        if (kDown & HidNpadButton_Plus) running = false;

        SDL_SetRenderDrawColor(renderer, 15, 15, 15, 255); 
        SDL_RenderClear(renderer);

        // --- SCREEN: MAIN MENU ---
        if (current_screen == SCREEN_MAIN_MENU) {
            if (kDown & HidNpadButton_Up) {
                main_selected--;
                if (main_selected < 0) main_selected = main_menu.size() - 1;
            }
            if (kDown & HidNpadButton_Down) {
                main_selected++;
                if (main_selected >= (int)main_menu.size()) main_selected = 0;
            }
            
            if (kDown & HidNpadButton_A) {
                sub_selected = 0;
                if (main_selected == 0) {
                    current_screen = SCREEN_ATMOSPHERE_MENU;
                    if (!g_atmosphere_loaded) {
                        load_atmosphere_data();
                    }
                }
                else if (main_selected == 1) current_screen = SCREEN_HEKATE_MENU;
                else if (main_selected == 2) current_screen = SCREEN_HOMEBREW_MENU;
            }

            // Draw Title
            if (title_font) {
                SDL_Surface* titleSurf = TTF_RenderUTF8_Solid(title_font, "Abyss Updater", color_white);
                if (titleSurf) {
                    SDL_Texture* titleTex = SDL_CreateTextureFromSurface(renderer, titleSurf);
                    SDL_Rect titleRect = {80, 60, titleSurf->w, titleSurf->h};
                    SDL_RenderCopy(renderer, titleTex, NULL, &titleRect);
                    SDL_DestroyTexture(titleTex);
                    SDL_FreeSurface(titleSurf);
                }
            }

            // Draw Main Categories
            if (font) {
                for (size_t i = 0; i < main_menu.size(); i++) {
                    SDL_Color textColor = ((int)i == main_selected) ? color_green : color_gray; 
                    char display_text[256];
                    if ((int)i == main_selected) {
                        sprintf(display_text, "> %s", main_menu[i].c_str());
                    } else {
                        sprintf(display_text, "  %s", main_menu[i].c_str());
                    }

                    SDL_Surface* itemSurf = TTF_RenderUTF8_Solid(font, display_text, textColor);
                    if (itemSurf) {
                        SDL_Texture* itemTex = SDL_CreateTextureFromSurface(renderer, itemSurf);
                        SDL_Rect itemRect = {100, 200 + ((int)i * 70), itemSurf->w, itemSurf->h};
                        SDL_RenderCopy(renderer, itemTex, NULL, &itemRect);
                        SDL_DestroyTexture(itemTex);
                        SDL_FreeSurface(itemSurf);
                    }
                }
                
                SDL_Surface* instSurf = TTF_RenderUTF8_Solid(font, "A: Select   |   +: Exit", color_dark_gray);
                if (instSurf) {
                    SDL_Texture* instTex = SDL_CreateTextureFromSurface(renderer, instSurf);
                    SDL_Rect instRect = {100, 620, instSurf->w, instSurf->h};
                    SDL_RenderCopy(renderer, instTex, NULL, &instRect);
                    SDL_DestroyTexture(instTex);
                    SDL_FreeSurface(instSurf);
                }
            }
        }
        
        // --- SCREEN: ATMOSPHERE MENU (Last 5 Releases) ---
        else if (current_screen == SCREEN_ATMOSPHERE_MENU) {
            if (kDown & HidNpadButton_B) current_screen = SCREEN_MAIN_MENU;
            
            if (!g_atmosphere_releases.empty()) {
                if (kDown & HidNpadButton_Up) {
                    sub_selected--;
                    if (sub_selected < 0) sub_selected = g_atmosphere_releases.size() - 1;
                }
                if (kDown & HidNpadButton_Down) {
                    sub_selected++;
                    if (sub_selected >= (int)g_atmosphere_releases.size()) sub_selected = 0;
                }
                
                if (kDown & HidNpadButton_A && !g_atmosphere_releases[sub_selected].url.empty()) {
                    if (worker.joinable()) worker.join();
                    
                    g_target_assets = {{g_atmosphere_releases[sub_selected].url, "atmosphere.zip"}};
                    g_target_version_tag = g_atmosphere_releases[sub_selected].tag;
                    g_is_downloading = true;
                    g_is_extracting = false;
                    g_is_finished = false;
                    g_progress = 0;
                    
                    worker = std::thread(worker_thread);
                    current_screen = SCREEN_UPDATING;
                }
            }

            if (title_font) {
                SDL_Surface* titleSurf = TTF_RenderUTF8_Solid(title_font, "Atmosphere Releases", color_white);
                if (titleSurf) {
                    SDL_Texture* titleTex = SDL_CreateTextureFromSurface(renderer, titleSurf);
                    SDL_Rect titleRect = {80, 50, titleSurf->w, titleSurf->h};
                    SDL_RenderCopy(renderer, titleTex, NULL, &titleRect);
                    SDL_DestroyTexture(titleTex);
                    SDL_FreeSurface(titleSurf);
                }
            }

            if (font) {
                char local_status[256];
                sprintf(local_status, "Current Status: %s", g_local_atmosphere_version.c_str());
                SDL_Surface* statSurf = TTF_RenderUTF8_Solid(font, local_status, color_green);
                if (statSurf) {
                    SDL_Texture* statTex = SDL_CreateTextureFromSurface(renderer, statSurf);
                    SDL_Rect statRect = {100, 120, statSurf->w, statSurf->h};
                    SDL_RenderCopy(renderer, statTex, NULL, &statRect);
                    SDL_DestroyTexture(statTex);
                    SDL_FreeSurface(statSurf);
                }

                for (size_t i = 0; i < g_atmosphere_releases.size(); i++) {
                    SDL_Color textColor = ((int)i == sub_selected) ? color_green : color_gray;
                    char display_text[512];
                    if ((int)i == sub_selected) {
                        sprintf(display_text, "> %s (%s)", g_atmosphere_releases[i].name.c_str(), g_atmosphere_releases[i].tag.c_str());
                    } else {
                        sprintf(display_text, "  %s (%s)", g_atmosphere_releases[i].name.c_str(), g_atmosphere_releases[i].tag.c_str());
                    }

                    SDL_Surface* itemSurf = TTF_RenderUTF8_Solid(font, display_text, textColor);
                    if (itemSurf) {
                        SDL_Texture* itemTex = SDL_CreateTextureFromSurface(renderer, itemSurf);
                        SDL_Rect itemRect = {100, 190 + ((int)i * 55), itemSurf->w, itemSurf->h};
                        SDL_RenderCopy(renderer, itemTex, NULL, &itemRect);
                        SDL_DestroyTexture(itemTex);
                        SDL_FreeSurface(itemSurf);
                    }
                }
                
                SDL_Surface* instSurf = TTF_RenderUTF8_Solid(font, "A: Install Selected   |   B: Back", color_dark_gray);
                if (instSurf) {
                    SDL_Texture* instTex = SDL_CreateTextureFromSurface(renderer, instSurf);
                    SDL_Rect instRect = {100, 620, instSurf->w, instSurf->h};
                    SDL_RenderCopy(renderer, instTex, NULL, &instRect);
                    SDL_DestroyTexture(instTex);
                    SDL_FreeSurface(instSurf);
                }
            }
        }
        
        // --- SCREEN: HEKATE MENU ---
        else if (current_screen == SCREEN_HEKATE_MENU) {
            if (kDown & HidNpadButton_B) current_screen = SCREEN_MAIN_MENU;
            
            if (kDown & HidNpadButton_A) {
                if (worker.joinable()) worker.join();
                
                std::vector<AssetInfo> assets = get_all_release_assets("https://api.github.com/repos/CTCaer/hekate/releases/latest", "sdmc:/hekate_api.json");
                if (assets.empty()) {
                    assets.push_back({"https://api.github.com/repos/CTCaer/hekate/releases/latest", "hekate.zip"});
                }

                g_target_assets = assets;
                g_is_downloading = true;
                g_is_extracting = false;
                g_is_finished = false;
                g_progress = 0;
                
                worker = std::thread(worker_thread);
                current_screen = SCREEN_UPDATING;
            }

            if (title_font) {
                SDL_Surface* titleSurf = TTF_RenderUTF8_Solid(title_font, "Hekate Updater", color_white);
                if (titleSurf) {
                    SDL_Texture* titleTex = SDL_CreateTextureFromSurface(renderer, titleSurf);
                    SDL_Rect titleRect = {80, 60, titleSurf->w, titleSurf->h};
                    SDL_RenderCopy(renderer, titleTex, NULL, &titleRect);
                    SDL_DestroyTexture(titleTex);
                    SDL_FreeSurface(titleSurf);
                }
            }

            if (font) {
                SDL_Surface* itemSurf = TTF_RenderUTF8_Solid(font, "> Update Hekate (Latest Release)", color_green);
                if (itemSurf) {
                    SDL_Texture* itemTex = SDL_CreateTextureFromSurface(renderer, itemSurf);
                    SDL_Rect itemRect = {100, 220, itemSurf->w, itemSurf->h};
                    SDL_RenderCopy(renderer, itemTex, NULL, &itemRect);
                    SDL_DestroyTexture(itemTex);
                    SDL_FreeSurface(itemSurf);
                }

                SDL_Surface* instSurf = TTF_RenderUTF8_Solid(font, "A: Update Now   |   B: Back", color_dark_gray);
                if (instSurf) {
                    SDL_Texture* instTex = SDL_CreateTextureFromSurface(renderer, instSurf);
                    SDL_Rect instRect = {100, 620, instSurf->w, instSurf->h};
                    SDL_RenderCopy(renderer, instTex, NULL, &instRect);
                    SDL_DestroyTexture(instTex);
                    SDL_FreeSurface(instSurf);
                }
            }
        }

        // --- SCREEN: HOMEBREW MENU ---
        else if (current_screen == SCREEN_HOMEBREW_MENU) {
            if (kDown & HidNpadButton_B) current_screen = SCREEN_MAIN_MENU;
            
            std::vector<HomebrewItem> homebrew_items = {
                {"EdiZon Overlay / App", "https://api.github.com/repos/WerWolv/EdiZon/releases/latest", "edizon.zip", "edizon"},
                {"Patches (fs-patch)", "https://api.github.com/repos/DefenderOfHyrule/fs-patch/releases/latest", "fs-patch.zip", "patches"},
                {"Sphaira", "https://api.github.com/repos/NaGaa95/sphaira/releases/latest", "sphaira.zip", "sphaira"},
                {"JKSV", "https://api.github.com/repos/J-D-K/JKSV/releases/latest", "jksv.zip", "jksv"},
                {"Checkpoint", "https://api.github.com/repos/bernardogiordano/checkpoint/releases/latest", "checkpoint.zip", "checkpoint"},
                {"CaptureSight", "https://api.github.com/repos/Insektaure/CaptureSight/releases/latest", "capturesight.zip", "capturesight"}
            };

            if (kDown & HidNpadButton_Up) {
                sub_selected--;
                if (sub_selected < 0) sub_selected = homebrew_items.size() - 1;
            }
            if (kDown & HidNpadButton_Down) {
                sub_selected++;
                if (sub_selected >= (int)homebrew_items.size()) sub_selected = 0;
            }

            if (kDown & HidNpadButton_A) {
                if (worker.joinable()) worker.join();
                
                std::vector<AssetInfo> assets = get_all_release_assets(homebrew_items[sub_selected].api_url, "sdmc:/hb_api.json");
                if (assets.empty()) {
                    assets.push_back({homebrew_items[sub_selected].api_url, homebrew_items[sub_selected].default_filename});
                }

                g_target_assets = assets;
                g_is_downloading = true;
                g_is_extracting = false;
                g_is_finished = false;
                g_progress = 0;
                
                worker = std::thread(worker_thread);
                current_screen = SCREEN_UPDATING;
            }

            if (title_font) {
                SDL_Surface* titleSurf = TTF_RenderUTF8_Solid(title_font, "Homebrew Apps & Patches", color_white);
                if (titleSurf) {
                    SDL_Texture* titleTex = SDL_CreateTextureFromSurface(renderer, titleSurf);
                    SDL_Rect titleRect = {80, 60, titleSurf->w, titleSurf->h};
                    SDL_RenderCopy(renderer, titleTex, NULL, &titleRect);
                    SDL_DestroyTexture(titleTex);
                    SDL_FreeSurface(titleSurf);
                }
            }

            if (font) {
                for (size_t i = 0; i < homebrew_items.size(); i++) {
                    bool installed = is_app_installed(homebrew_items[i].keyword);
                    std::string status_str = installed ? "[Installed]" : "[Not Installed]";
                    
                    SDL_Color textColor = ((int)i == sub_selected) ? color_green : color_gray;
                    char display_text[512];
                    if ((int)i == sub_selected) {
                        sprintf(display_text, "> %s  %s", homebrew_items[i].name.c_str(), status_str.c_str());
                    } else {
                        sprintf(display_text, "  %s  %s", homebrew_items[i].name.c_str(), status_str.c_str());
                    }

                    SDL_Surface* itemSurf = TTF_RenderUTF8_Solid(font, display_text, textColor);
                    if (itemSurf) {
                        SDL_Texture* itemTex = SDL_CreateTextureFromSurface(renderer, itemSurf);
                        SDL_Rect itemRect = {100, 180 + ((int)i * 50), itemSurf->w, itemSurf->h};
                        SDL_RenderCopy(renderer, itemTex, NULL, &itemRect);
                        SDL_DestroyTexture(itemTex);
                        SDL_FreeSurface(itemSurf);
                    }
                }

                SDL_Surface* instSurf = TTF_RenderUTF8_Solid(font, "A: Install Selected   |   B: Back", color_dark_gray);
                if (instSurf) {
                    SDL_Texture* instTex = SDL_CreateTextureFromSurface(renderer, instSurf);
                    SDL_Rect instRect = {100, 620, instSurf->w, instSurf->h};
                    SDL_RenderCopy(renderer, instTex, NULL, &instRect);
                    SDL_DestroyTexture(instTex);
                    SDL_FreeSurface(instSurf);
                }
            }
        }
        
        // --- SCREEN: UPDATING PROGRESS ---
        else if (current_screen == SCREEN_UPDATING) {
            if (g_is_finished && (kDown & HidNpadButton_B)) {
                current_screen = SCREEN_MAIN_MENU;
            }

            SDL_Rect outline = {240, 335, 800, 50};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDrawRect(renderer, &outline);

            int current_progress = g_progress;
            if (current_progress < 0) current_progress = 0;
            if (current_progress > 100) current_progress = 100;

            SDL_Rect fill = {242, 337, (int)(796 * (current_progress / 100.0f)), 46};
            
            if (g_is_finished) {
                SDL_SetRenderDrawColor(renderer, 0, 255, 150, 255); 
            } else if (g_is_extracting) {
                SDL_SetRenderDrawColor(renderer, 255, 165, 0, 255); 
            } else {
                SDL_SetRenderDrawColor(renderer, 0, 150, 255, 255); 
            }
            
            if (current_progress > 0) SDL_RenderFillRect(renderer, &fill);
            
            if (font) {
                char status_text[256];
                if (g_is_finished) {
                    sprintf(status_text, "Update complete! Press B to return.");
                } else if (g_is_extracting) {
                    sprintf(status_text, "Extracting files... %d%%", current_progress);
                } else if (g_is_downloading) {
                    sprintf(status_text, "Downloading package... %d%%", current_progress);
                } else {
                    sprintf(status_text, "Connecting...");
                }

                SDL_Surface* textSurf = TTF_RenderUTF8_Solid(font, status_text, color_white);
                if (textSurf) {
                    SDL_Texture* textTex = SDL_CreateTextureFromSurface(renderer, textSurf);
                    SDL_Rect textRect = {240, 295, textSurf->w, textSurf->h};
                    SDL_RenderCopy(renderer, textTex, NULL, &textRect);
                    SDL_DestroyTexture(textTex);
                    SDL_FreeSurface(textSurf);
                }
            }
        }
        
        SDL_RenderPresent(renderer);
        SDL_Delay(16); 
    }

    // Clean up
    if (worker.joinable()) worker.join();
    
    if (font) TTF_CloseFont(font);
    if (title_font) TTF_CloseFont(title_font);
    
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    curl_global_cleanup();
    socketExit();
    romfsExit(); 
    
    return 0;
}