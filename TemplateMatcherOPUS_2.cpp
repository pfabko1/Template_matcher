// TemplateMatcher.cpp - Hlavný program pre template matching s učením
// Kompiluj s: cl.exe /O2 /arch:AVX2 TemplateMatcher.cpp /link user32.lib gdi32.lib
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <immintrin.h>  // AVX2
#include <emmintrin.h>  // SSE2
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <ctime>

// PNG/JPG/BMP loading - použi stb_image.h (single header library)
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
// Stiahni z: https://github.com/nothings/stb/blob/master/stb_image.h
// #include "stb_image.h"
// #include "stb_image_write.h"

// Pre tento príklad použijem Windows BMP
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// Pre C++17 a novšie
#include <random>
#include <algorithm>

// ===== KONFIGURÁCIA =====
constexpr int TEMPLATE_SIZE = 20;  // Veľkosť šablóny 20x20
constexpr int MAX_TEMPLATES = 500;  // Max počet šablón
constexpr int SCREEN_WIDTH = 1920;
constexpr int SCREEN_HEIGHT = 1080;
std::thread displayThread;
// ===== GLOBÁLNE PREMENNÉ =====
struct Template {
    std::vector<uint8_t> data;  // BGRA data (4 bajty na pixel)
    std::string filename;
    int width = TEMPLATE_SIZE;
    int height = TEMPLATE_SIZE;
    bool active = true;  // Či sa má testovať
};

struct SearchRegion {
    int x, y, width, height;
    std::string name;
    bool active = true;
};

struct MatchResult {
    int templateId;
    int x, y;
    float score;
    std::chrono::steady_clock::time_point timestamp;
};
void DrawHitVisualization();

// Globálne nastavenia
struct Settings {
    bool clickOnMatch = false;
    bool doubleClick = false;
    int tolerance = 10;  // 0-255, nižšie = presnejšie
    int earlyPixelCount = 100;  // Počet pixelov pre early rejection
    bool randomPixelTest = false;  // Random vs sekvenčné testovanie
    bool useAVX2 = true;  // AVX2 vs SSE2
    bool showFPS = true;
    bool enableLearning = true;
    int currentRegionSet = 0;  // Ktorý set regiónov používame
} g_settings;

// Globálne dáta
std::vector<Template> g_templates;
std::vector<SearchRegion> g_searchRegions;
std::vector<MatchResult> g_lastMatches;
std::atomic<bool> g_running(true);
std::atomic<int> g_fps(0);
std::atomic<float> g_lastProcessTime(0.0f);
std::atomic<bool> g_searchActive(false);
std::atomic<int> g_currentMatchIndex(0);
// Učenie - štatistiky pre každú šablónu
struct TemplateStats {
    int hitCount = 0;
    std::vector<POINT> hitPositions;  // História pozícií
    POINT avgPosition = { 0, 0 };
    float probability = 0.0f;
    int regionPreference[10] = { 0 };  // Ktoré regióny preferuje
    std::chrono::steady_clock::time_point lastHitTime;  // <-- PRIDAJ TOTO
};
std::vector<TemplateStats> g_templateStats;

// ===== POMOCNÉ FUNKCIE =====

// Načíta BMP súbor (32-bit BGRA)
bool LoadBMP32(const std::string& filename, std::vector<uint8_t>& data, int& width, int& height) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) return false;

    // BMP header
    char header[54];
    file.read(header, 54);

    width = *(int*)&header[18];
    height = *(int*)&header[22];
    int bpp = *(short*)&header[28];

    if (bpp != 32) {
        std::cerr << "Podporované sú len 32-bit BMP súbory!" << std::endl;
        return false;
    }

    // Načítaj pixel data
    int dataSize = width * height * 4;
    data.resize(dataSize);
    file.seekg(*(int*)&header[10], std::ios::beg);
    file.read((char*)data.data(), dataSize);

    // BMP je uložené bottom-up, pretoč to
    for (int y = 0; y < height / 2; y++) {
        for (int x = 0; x < width * 4; x++) {
            std::swap(data[y * width * 4 + x],
                data[(height - 1 - y) * width * 4 + x]);
        }
    }

    return true;
}

// Uloží 32-bit BMP
bool SaveBMP32(const std::string& filename, const uint8_t* data, int width, int height) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;

    // BMP header
    int fileSize = 54 + width * height * 4;
    char header[54] = { 0 };

    // Signature
    header[0] = 'B'; header[1] = 'M';
    // File size
    *(int*)&header[2] = fileSize;
    // Data offset
    *(int*)&header[10] = 54;
    // Info header size
    *(int*)&header[14] = 40;
    // Width, height
    *(int*)&header[18] = width;
    *(int*)&header[22] = height;
    // Planes
    *(short*)&header[26] = 1;
    // BPP
    *(short*)&header[28] = 32;

    file.write(header, 54);

    // Zapíš pixel data (bottom-up)
    for (int y = height - 1; y >= 0; y--) {
        file.write((char*)&data[y * width * 4], width * 4);
    }

    return true;
}

void LoadConfig(const std::string& filename = "config.ini") {
    std::ifstream file(filename);
    if (!file) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("=") != std::string::npos) {
            std::string key = line.substr(0, line.find("="));
            std::string value = line.substr(line.find("=") + 1);

            if (key == "ClickOnMatch") g_settings.clickOnMatch = std::stoi(value);
            else if (key == "DoubleClick") g_settings.doubleClick = std::stoi(value);
            else if (key == "Tolerance") g_settings.tolerance = std::stoi(value);
            else if (key == "EarlyPixelCount") g_settings.earlyPixelCount = std::stoi(value);
            else if (key == "RandomPixelTest") g_settings.randomPixelTest = std::stoi(value);
            else if (key == "UseAVX2") g_settings.useAVX2 = std::stoi(value);
            else if (key == "ShowFPS") g_settings.showFPS = std::stoi(value);
            else if (key == "EnableLearning") g_settings.enableLearning = std::stoi(value);
        }
    }
}
void SaveConfig(const std::string& filename = "config.ini") {
    std::ofstream file(filename);
    if (!file) return;

    file << "[Settings]\n";
    file << "ClickOnMatch=" << g_settings.clickOnMatch << "\n";
    file << "DoubleClick=" << g_settings.doubleClick << "\n";
    file << "Tolerance=" << g_settings.tolerance << "\n";
    file << "EarlyPixelCount=" << g_settings.earlyPixelCount << "\n";
    file << "RandomPixelTest=" << g_settings.randomPixelTest << "\n";
    file << "UseAVX2=" << g_settings.useAVX2 << "\n";
    file << "ShowFPS=" << g_settings.showFPS << "\n";
    file << "EnableLearning=" << g_settings.enableLearning << "\n";
}
// Uloženie štatistík učenia
void SaveLearningStats(const std::string& filename = "learning_stats.dat") {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return;

    // Ulož počet šablón
    size_t count = g_templateStats.size();
    file.write((char*)&count, sizeof(count));

    // Pre každú šablónu
    for (size_t i = 0; i < count; i++) {
        const auto& stats = g_templateStats[i];

        // Ulož základné dáta
        file.write((char*)&stats.hitCount, sizeof(stats.hitCount));
        file.write((char*)&stats.avgPosition, sizeof(stats.avgPosition));
        file.write((char*)&stats.probability, sizeof(stats.probability));

        // Ulož históriu pozícií (max posledných 100)
        size_t histSize = min(stats.hitPositions.size(), (size_t)100);
        file.write((char*)&histSize, sizeof(histSize));

        // Ulož len posledných 100 pozícií
        size_t startIdx = stats.hitPositions.size() > 100 ? stats.hitPositions.size() - 100 : 0;
        for (size_t j = startIdx; j < stats.hitPositions.size(); j++) {
            file.write((char*)&stats.hitPositions[j], sizeof(POINT));
        }

        // Ulož čas posledného hitu (ako počet sekúnd od epochy)
        auto timeSinceEpoch = stats.lastHitTime.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeSinceEpoch).count();
        file.write((char*)&seconds, sizeof(seconds));
    }

    std::cout << "Štatistiky učenia uložené do: " << filename << std::endl;
}

// Načítanie štatistík učenia
void LoadLearningStats(const std::string& filename = "learning_stats.dat") {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cout << "Súbor so štatistikami neexistuje, začínam s prázdnymi." << std::endl;
        return;
    }

    size_t count;
    file.read((char*)&count, sizeof(count));

    // Uisti sa že máme správny počet štatistík
    g_templateStats.resize(max(count, g_templates.size()));

    for (size_t i = 0; i < count && i < g_templateStats.size(); i++) {
        auto& stats = g_templateStats[i];

        // Načítaj základné dáta
        file.read((char*)&stats.hitCount, sizeof(stats.hitCount));
        file.read((char*)&stats.avgPosition, sizeof(stats.avgPosition));
        file.read((char*)&stats.probability, sizeof(stats.probability));

        // Načítaj históriu
        size_t histSize;
        file.read((char*)&histSize, sizeof(histSize));

        stats.hitPositions.clear();
        for (size_t j = 0; j < histSize; j++) {
            POINT p;
            file.read((char*)&p, sizeof(POINT));
            stats.hitPositions.push_back(p);
        }

        // Načítaj čas
        long long seconds;
        file.read((char*)&seconds, sizeof(seconds));
        stats.lastHitTime = std::chrono::steady_clock::time_point(std::chrono::seconds(seconds));
    }

    std::cout << "Načítané štatistiky pre " << count << " šablón." << std::endl;
}
// Načíta všetky obrázky z adresára
void LoadTemplates() {
    g_templates.clear();
    g_templateStats.clear();
    // Načítaj štatistiky učenia
    LoadLearningStats();  // <-- PRIDAJ TOTO

    // Načítaj konfiguráciu
    LoadConfig();  // <-- PRIDAJ AJ TOTO

    std::string path = "./obr/";

    // Vytvor adresár ak neexistuje
    std::filesystem::create_directories(path);

    // Načítaj všetky .bmp súbory
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".bmp") {
            Template tmpl;
            int width, height;

            if (LoadBMP32(entry.path().string(), tmpl.data, width, height)) {
                if (width == TEMPLATE_SIZE && height == TEMPLATE_SIZE) {
                    tmpl.filename = entry.path().filename().string();
                    tmpl.width = width;
                    tmpl.height = height;
                    g_templates.push_back(tmpl);
                    g_templateStats.push_back(TemplateStats());

                    std::cout << "Načítaná šablóna: " << tmpl.filename << std::endl;
                }
            }
        }
    }

    std::cout << "Načítaných šablón: " << g_templates.size() << std::endl;
}

// Zachytí screenshot
std::vector<uint8_t> CaptureScreen(int x, int y, int width, int height) {
    std::vector<uint8_t> data(width * height * 4);

    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDC, width, height);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, bitmap);

    BitBlt(memDC, 0, 0, width, height, screenDC, x, y, SRCCOPY);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    GetDIBits(memDC, bitmap, 0, height, data.data(), &bmi, DIB_RGB_COLORS);

    SelectObject(memDC, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);

    return data;
}

// SSE2 template matching s early rejection
float MatchTemplateSSE2(const uint8_t* image, int imgStride,
    const uint8_t* tmpl, int tolerance, int earlyPixels) {
    int totalDiff = 0;
    int pixelsTested = 0;

    for (int y = 0; y < TEMPLATE_SIZE; y++) {
        for (int x = 0; x < TEMPLATE_SIZE; x += 4) {
            // Kontrola či nezájdeme mimo hranice
            int pixelsToProcess = min(4, TEMPLATE_SIZE - x);

            if (pixelsToProcess == 4) {
                // Načítaj 4 pixely z obrazu a šablóny
                __m128i imgPixels = _mm_loadu_si128((__m128i*) & image[y * imgStride + x * 4]);
                __m128i tmplPixels = _mm_loadu_si128((__m128i*) & tmpl[y * TEMPLATE_SIZE * 4 + x * 4]);

                // Vypočítaj absolútne rozdiely
                __m128i diff = _mm_sad_epu8(imgPixels, tmplPixels);

                // Akumuluj rozdiely
                totalDiff += _mm_cvtsi128_si32(diff) + _mm_extract_epi32(diff, 2);
                pixelsTested += 4;
            }
            else {
                // Spracuj zvyšné pixely manuálne
                for (int i = 0; i < pixelsToProcess; i++) {
                    for (int c = 0; c < 4; c++) {
                        int imgVal = image[y * imgStride + (x + i) * 4 + c];
                        int tmplVal = tmpl[y * TEMPLATE_SIZE * 4 + (x + i) * 4 + c];
                        totalDiff += abs(imgVal - tmplVal);
                    }
                    pixelsTested++;
                }
            }

            // Early rejection
            if (pixelsTested >= earlyPixels && totalDiff > tolerance * pixelsTested * 4) {
                return FLT_MAX;
            }
        }
    }

    return (float)totalDiff / (TEMPLATE_SIZE * TEMPLATE_SIZE * 4);
}
// AVX2 template matching s early rejection
float MatchTemplateAVX2(const uint8_t* image, int imgStride,
    const uint8_t* tmpl, int tolerance, int earlyPixels) {
    int totalDiff = 0;
    int pixelsTested = 0;

    for (int y = 0; y < TEMPLATE_SIZE; y++) {
        int x = 0;

        // Spracuj po 8 pixelov pokiaľ môžeme
        for (; x <= TEMPLATE_SIZE - 8; x += 8) {
            __m256i imgPixels = _mm256_loadu_si256((__m256i*) & image[y * imgStride + x * 4]);
            __m256i tmplPixels = _mm256_loadu_si256((__m256i*) & tmpl[y * TEMPLATE_SIZE * 4 + x * 4]);

            __m256i diff = _mm256_sad_epu8(imgPixels, tmplPixels);

            totalDiff += _mm256_extract_epi32(diff, 0) + _mm256_extract_epi32(diff, 2) +
                _mm256_extract_epi32(diff, 4) + _mm256_extract_epi32(diff, 6);

            pixelsTested += 8;

            // Early rejection
            if (pixelsTested >= earlyPixels && totalDiff > tolerance * pixelsTested * 4) {
                return FLT_MAX;
            }
        }

        // Dokonči zvyšné pixely pomocou SSE2
        for (; x < TEMPLATE_SIZE; x += 4) {
            __m128i imgPixels = _mm_loadu_si128((__m128i*) & image[y * imgStride + x * 4]);
            __m128i tmplPixels = _mm_loadu_si128((__m128i*) & tmpl[y * TEMPLATE_SIZE * 4 + x * 4]);

            __m128i diff = _mm_sad_epu8(imgPixels, tmplPixels);
            totalDiff += _mm_cvtsi128_si32(diff) + _mm_extract_epi32(diff, 2);

            pixelsTested += 4;
        }
    }

    return (float)totalDiff / (TEMPLATE_SIZE * TEMPLATE_SIZE * 4);
}
// Hlavná funkcia pre hľadanie šablón
void FindTemplates() {
    auto startTime = std::chrono::steady_clock::now();

    g_lastMatches.clear();

    // Pre každý región
    for (const auto& region : g_searchRegions) {
        if (!region.active) continue;

        // Zachyť screenshot regiónu
        auto screenshot = CaptureScreen(region.x, region.y, region.width, region.height);

        // Hľadaj každú šablónu
        for (int t = 0; t < g_templates.size(); t++) {
            if (!g_templates[t].active) continue;

            float bestScore = FLT_MAX;
            int bestX = -1, bestY = -1;

            // Skenuj celý región
            for (int y = 0; y <= region.height - TEMPLATE_SIZE; y++) {
                for (int x = 0; x <= region.width - TEMPLATE_SIZE; x++) {
                    float score;

                    if (g_settings.useAVX2) {
                        score = MatchTemplateAVX2(
                            &screenshot[(y * region.width + x) * 4],
                            region.width * 4,
                            g_templates[t].data.data(),
                            g_settings.tolerance,
                            g_settings.earlyPixelCount
                        );
                    }
                    else {
                        score = MatchTemplateSSE2(
                            &screenshot[(y * region.width + x) * 4],
                            region.width * 4,
                            g_templates[t].data.data(),
                            g_settings.tolerance,
                            g_settings.earlyPixelCount
                        );
                    }

                    if (score < bestScore) {
                        bestScore = score;
                        bestX = x;
                        bestY = y;
                    }
                }
            }

            // Ak sme našli dobrú zhodu
            if (bestScore < g_settings.tolerance) {
                MatchResult match;
                match.templateId = t;
                match.x = region.x + bestX + TEMPLATE_SIZE / 2;  // Stred šablóny
                match.y = region.y + bestY + TEMPLATE_SIZE / 2;
                match.score = bestScore;
                match.timestamp = std::chrono::steady_clock::now();

                g_lastMatches.push_back(match);

                // Aktualizuj štatistiky (učenie)
                if (g_settings.enableLearning) {
                    g_templateStats[t].hitCount++;
                    g_templateStats[t].lastHitTime = std::chrono::steady_clock::now();  // <-- PRIDAJ TOTO
                    g_templateStats[t].hitPositions.push_back({ match.x, match.y });

                    // Prepočítaj priemernú pozíciu
                    long sumX = 0, sumY = 0;
                    for (const auto& pos : g_templateStats[t].hitPositions) {
                        sumX += pos.x;
                        sumY += pos.y;
                    }
                    g_templateStats[t].avgPosition.x = (LONG)(sumX / g_templateStats[t].hitPositions.size());
                    g_templateStats[t].avgPosition.y = (LONG)(sumY / g_templateStats[t].hitPositions.size());
                }
            }
        }
    }

    // Aktualizuj FPS       
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    g_lastProcessTime = duration.count() / 1000.0f;  // ms

    static int frameCount = 0;
    static auto lastFPSUpdate = std::chrono::steady_clock::now();
    frameCount++;

    if (std::chrono::duration_cast<std::chrono::seconds>(endTime - lastFPSUpdate).count() >= 1) {
        g_fps = frameCount;
        frameCount = 0;
        lastFPSUpdate = endTime;
    }
}

// Klikni na pozíciu
void ClickAt(int x, int y, bool doubleClick = false) {
    SetCursorPos(x, y);

    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);

    if (doubleClick) {
        Sleep(50);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
}

// Uloží aktuálne regióny do súboru
void SaveRegions(const std::string& filename) {
    std::ofstream file(filename);
    if (!file) return;

    file << g_searchRegions.size() << std::endl;
    for (const auto& region : g_searchRegions) {
        file << region.x << " " << region.y << " "
            << region.width << " " << region.height << " "
            << region.active << " " << region.name << std::endl;
    }
}

// Načíta regióny zo súboru
void LoadRegions(const std::string& filename) {
    std::ifstream file(filename);
    if (!file) return;

    g_searchRegions.clear();

    int count;
    file >> count;

    for (int i = 0; i < count; i++) {
        SearchRegion region;
        file >> region.x >> region.y >> region.width >> region.height
            >> region.active;
        file.ignore();  // Skip whitespace
        std::getline(file, region.name);
        g_searchRegions.push_back(region);
    }
}

// Vytvorí nový región myšou, vráti true ak bol vytvorený, false ak zrušený ESC
bool CreateRegionByMouse() {
    std::cout << "Klikni a ťahaj pre vytvorenie regiónu (ESC pre zrušenie)...\n";

    POINT start, end;
    // Počkáme na uvoľnenie všetkých týchto kláves
    while (GetAsyncKeyState(VK_LBUTTON) & 0x8000 ||
        GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        Sleep(10);
    }
    Sleep(100);

    // Čakáme na prvý down ľavého tlačidla
    while (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        Sleep(10);
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            std::cout << "Vytvorenie regiónu zrušené.\n";
            // Počkáme na uvoľnenie ESC
            while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                Sleep(10);
            }
            return false;
        }
    }
    GetCursorPos(&start);
    // Pripravíme DC pre invert režim
    HDC screenDC = GetDC(NULL);
    SetROP2(screenDC, R2_NOT);
    POINT lastEnd = start;

    // Ťahanie - invert kreslíme a zmazávame staré
    while (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        // Ak v priebehu držania stlačíme ESC, zrušíme a upraceme kreslenie
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            // Zmažeme posledný škrabanec
            Rectangle(screenDC, start.x, start.y, lastEnd.x, lastEnd.y);
            ReleaseDC(NULL, screenDC);
            std::cout << "Vytvorenie regiónu zrušené.\n";
            // počkáme na uvoľnenie ESC
            while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                Sleep(10);
            }
            return false;
        }

        GetCursorPos(&end);
        // invert erase starej a nakresli novej
        Rectangle(screenDC, start.x, start.y, lastEnd.x, lastEnd.y);
        Rectangle(screenDC, start.x, start.y, end.x, end.y);
        lastEnd = end;
        Sleep(10);
    }

    // Po uvoľnení tlačidla ešte raz invert vyčistíme
    Rectangle(screenDC, start.x, start.y, lastEnd.x, lastEnd.y);
    ReleaseDC(NULL, screenDC);
    // Vypočítame finálnu oblasť
    SearchRegion newRegion;
    newRegion.x = min(start.x, end.x);
    newRegion.y = min(start.y, end.y);
    newRegion.width = abs(end.x - start.x);
    newRegion.height = abs(end.y - start.y);
    newRegion.name = "Region_" + std::to_string(g_searchRegions.size());
    newRegion.active = true;

    // Overenie minimálnej veľkosti
    if (newRegion.width > 10 && newRegion.height > 10) {
        g_searchRegions.push_back(newRegion);
        std::cout << "Vytvorený región: " << newRegion.name
            << " [" << newRegion.x << "," << newRegion.y
            << " " << newRegion.width << "x" << newRegion.height << "]\n"
            << "Celkovo regiónov: " << g_searchRegions.size() << "\n";
        return true;
    }
    else {
        std::cout << "Región príliš malý, nebol uložený.\n";
        return false;
    }
}

// Zachyť šablónu z pozície myši
void CaptureTemplateAtMouse() {
    POINT mousePos;
    GetCursorPos(&mousePos);

    // Zachyť 20x20 oblasť okolo myši
    int x = mousePos.x - TEMPLATE_SIZE / 2;
    int y = mousePos.y - TEMPLATE_SIZE / 2;

    auto screenshot = CaptureScreen(x, y, TEMPLATE_SIZE, TEMPLATE_SIZE);

    // Generuj unikátny názov súboru
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "./obr/template_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S")
        << "_" << g_templates.size() << ".bmp";

    if (SaveBMP32(ss.str(), screenshot.data(), TEMPLATE_SIZE, TEMPLATE_SIZE)) {
        // Pridaj do zoznamu šablón
        Template tmpl;
        tmpl.data = screenshot;
        tmpl.filename = ss.str();
        tmpl.width = TEMPLATE_SIZE;
        tmpl.height = TEMPLATE_SIZE;
        g_templates.push_back(tmpl);
        g_templateStats.push_back(TemplateStats());

        std::cout << "Šablóna uložená: " << ss.str() << std::endl;
    }
}

// Hlavné menu
void ShowMenu() {
    std::cout << "\n========== TEMPLATE MATCHER ==========\n";
    std::cout << "1. Spustiť/Zastaviť hľadanie\n";
    std::cout << "2. Nastaviť klikanie (aktuálne: " << (g_settings.clickOnMatch ? "ZAP" : "VYP") << ")\n";
    std::cout << "3. Prepnúť double-click (aktuálne: " << (g_settings.doubleClick ? "ZAP" : "VYP") << ")\n";
    std::cout << "4. Tolerancia: " << g_settings.tolerance << " (T/Y pre zmenu)\n";
    std::cout << "5. Early pixels: " << g_settings.earlyPixelCount << " (E/R pre zmenu)\n";
    std::cout << "6. Prepnúť AVX2/SSE2 (aktuálne: " << (g_settings.useAVX2 ? "AVX2" : "SSE2") << ")\n";
    std::cout << "7. Vytvoriť nový región myšou\n";
    std::cout << "8. Uložiť regióny\n";
    std::cout << "9. Načítať regióny\n";
    std::cout << "0. Zobraziť FPS (aktuálne: " << (g_settings.showFPS ? "ZAP" : "VYP") << ")\n";
    std::cout << "V. Vizualizácia hitov (zobrazí krížiky)\n";
    std::cout << "CTRL - Zachytiť šablónu z pozície myši\n";
    std::cout << "ESC - Ukončiť program\n";
    std::cout << "=====================================\n";
    std::cout << "\nPre akciu stlač príslušnú klávesu...\n";
}

// Thread pre spracovanie
void ProcessingThread() {
    while (g_running) {
        if (g_searchActive && !g_searchRegions.empty() && !g_templates.empty()) {  // <-- použij g_searchActive
            FindTemplates();

            // Ak máme zhody a je zapnuté klikanie
            if (g_settings.clickOnMatch && !g_lastMatches.empty()) {
                // Cykluj cez všetky zhody
                int idx = g_currentMatchIndex % g_lastMatches.size();
                ClickAt(g_lastMatches[idx].x, g_lastMatches[idx].y, g_settings.doubleClick);
                g_currentMatchIndex++;
            }
        }

        Sleep(16);  // ~60 FPS
    }
}

// Thread pre zobrazenie FPS
void DisplayThread() {
    while (g_running) {
        if (g_settings.showFPS) {
            system("cls");
            ShowMenu();
            std::cout << "\n--- STAV ---\n";
            std::cout << "FPS: " << g_fps << "\n";
            std::cout << "Čas spracovania: " << g_lastProcessTime << " ms\n";
            std::cout << "Načítané šablóny: " << g_templates.size() << "\n";
            std::cout << "Aktívne regióny: " << g_searchRegions.size() << "\n";
            std::cout << "Posledné zhody: " << g_lastMatches.size() << "\n";

            // Zobraz top 5 najčastejších šablón
            if (g_settings.enableLearning && !g_templateStats.empty()) {
                std::cout << "\n--- TOP ŠABLÓNY ---\n";
                std::vector<std::pair<int, int>> sorted;
                for (int i = 0; i < (int)g_templateStats.size(); i++) {
                    sorted.push_back({ g_templateStats[i].hitCount, i });
                }
                std::sort(sorted.rbegin(), sorted.rend());

                for (int i = 0; i < min(5, (int)sorted.size()); i++) {
                    int tid = sorted[i].second;
                    std::cout << g_templates[tid].filename << ": "
                        << sorted[i].first << " hitov\n";
                }
            }
        }

        Sleep(1000);  // Update každú sekundu
    }
}

// Hlavná funkcia
int main() {
    std::cout << "Template Matcher v1.0\n";
    std::cout << "Načítavam šablóny...\n";

    // Načítaj šablóny
    LoadTemplates();

    // Skús načítať posledné regióny
    LoadRegions("last_regions.txt");

    if (g_searchRegions.empty()) {
        std::cout << "Žiadne regióny nenačítané. Vytvor nové pomocou menu.\n";

 
    }

    ShowMenu();

    // Spusti threads
    std::thread processingThread(ProcessingThread);
    std::thread displayThread(DisplayThread);

    // Hlavný input loop
    bool searchActive = false;

    // Vypni display na začiatku, aby neinterferoval
    g_settings.showFPS = false;

    while (g_running) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            g_running = false;
            break;
        }

        // CTRL pre zachytenie šablóny
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
            std::cout << "\nZachytávam šablónu..." << std::endl;
            CaptureTemplateAtMouse();
            Sleep(500);  // Debounce
        }
        if (GetAsyncKeyState('M') & 0x8000) {
            system("cls");
            ShowMenu();
            Sleep(200);
        }
        // Numerické klávesy pre menu
        
        if (GetAsyncKeyState('1') & 0x8000) {
            g_searchActive = !g_searchActive;  // <-- použij globálnu premennú
            std::cout << "\nHľadanie: " << (g_searchActive ? "ZAPNUTÉ" : "VYPNUTÉ") << std::endl;
            Sleep(200);
        }
        if (GetAsyncKeyState('2') & 0x8000) {
            g_settings.clickOnMatch = !g_settings.clickOnMatch;
            std::cout << "\nKlikanie: " << (g_settings.clickOnMatch ? "ZAPNUTÉ" : "VYPNUTÉ") << std::endl;
            Sleep(200);
        }

        if (GetAsyncKeyState('3') & 0x8000) {
            g_settings.doubleClick = !g_settings.doubleClick;
            std::cout << "\nDouble-click: " << (g_settings.doubleClick ? "ZAPNUTÉ" : "VYPNUTÉ") << std::endl;
            Sleep(200);
        }

        // T/Y pre toleranciu
        if (GetAsyncKeyState('T') & 0x8000) {
            g_settings.tolerance = max(0, g_settings.tolerance - 5);
            std::cout << "\nTolerancia: " << g_settings.tolerance << std::endl;
            Sleep(100);
        }
        if (GetAsyncKeyState('Y') & 0x8000) {
            g_settings.tolerance = min(255, g_settings.tolerance + 5);
            std::cout << "\nTolerancia: " << g_settings.tolerance << std::endl;
            Sleep(100);
        }

        // E/R pre early pixels
        if (GetAsyncKeyState('E') & 0x8000) {
            g_settings.earlyPixelCount = max(10, g_settings.earlyPixelCount - 10);
            std::cout << "\nEarly pixels: " << g_settings.earlyPixelCount << std::endl;
            Sleep(100);
        }
        if (GetAsyncKeyState('R') & 0x8000) {
            g_settings.earlyPixelCount = min(400, g_settings.earlyPixelCount + 10);
            std::cout << "\nEarly pixels: " << g_settings.earlyPixelCount << std::endl;
            Sleep(100);
        }

        if (GetAsyncKeyState('6') & 0x8000) {
            g_settings.useAVX2 = !g_settings.useAVX2;
            std::cout << "\nPoužívam: " << (g_settings.useAVX2 ? "AVX2" : "SSE2") << std::endl;
            Sleep(200);
        }

       
        if (GetAsyncKeyState('7') & 0x8000) {
            std::cout << "\nVytváram región..." << std::endl;
            CreateRegionByMouse();
            Sleep(500);  // Dlhšia pauza po vytvorení

            // Počkaj kým sa pustí klávesa 7
            while (GetAsyncKeyState('7') & 0x8000) {
                Sleep(10);
            }
        }
       // CreateRegionByMouse();
        Sleep(200);
    }

    //if (GetAsyncKeyState('8') & 0x8000) {
    //    // Generuj názov súboru s dátumom
    //    auto now = std::chrono::system_clock::now();
    //    auto time_t = std::chrono::system_clock::to_time_t(now);
    //    std::stringstream ss;
    //    ss << "regions_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S") << ".txt";
    //    SaveRegions(ss.str());
    //    std::cout << "\nRegióny uložené do: " << ss.str() << std::endl;
    //    Sleep(200);
    //}

    //if (GetAsyncKeyState('9') & 0x8000) {
    //    // Zobraz dostupné súbory s regiónmi
    //    std::cout << "\nDostupné súbory s regiónmi:\n";
    //    int fileIndex = 0;
    //    std::vector<std::string> regionFiles;

    //    for (const auto& entry : std::filesystem::directory_iterator(".")) {
    //        if (entry.path().extension() == ".txt" &&
    //            entry.path().filename().string().find("regions_") == 0) {
    //            std::cout << fileIndex++ << ": " << entry.path().filename() << std::endl;
    //            regionFiles.push_back(entry.path().filename().string());
    //        }
    //    }

    //    if (!regionFiles.empty()) {
    //        std::cout << "Zadaj číslo súboru: ";
    //        int choice;
    //        std::cin >> choice;

    //        if (choice >= 0 && choice < regionFiles.size()) {
    //            LoadRegions(regionFiles[choice]);
    //            std::cout << "Načítané regióny z: " << regionFiles[choice] << std::endl;
    //        }
    //    }
    //    Sleep(200);
    //}

    //if (GetAsyncKeyState('0') & 0x8000) {
    //    g_settings.showFPS = !g_settings.showFPS;
    //    if (!g_settings.showFPS) {
    //        system("cls");
    //        ShowMenu();
    //    }
    //    Sleep(200);
    //}
    if (GetAsyncKeyState('8') & 0x8000) {
        // Generuj názov súboru s dátumom
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << "regions_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S") << ".txt";
        SaveRegions(ss.str());
        std::cout << "\nRegióny uložené do: " << ss.str() << std::endl;
        Sleep(200);
    }

    if (GetAsyncKeyState('9') & 0x8000) {
        // Zobraz dostupné súbory s regiónmi
        std::cout << "\nDostupné súbory s regiónmi:\n";
        int fileIndex = 0;
        std::vector<std::string> regionFiles;

        for (const auto& entry : std::filesystem::directory_iterator(".")) {
            if (entry.path().extension() == ".txt" &&
                entry.path().filename().string().find("regions_") == 0) {
                std::cout << fileIndex++ << ": " << entry.path().filename() << std::endl;
                regionFiles.push_back(entry.path().filename().string());
            }
        }

        if (!regionFiles.empty()) {
            std::cout << "Zadaj číslo súboru: ";
            int choice;
            std::cin >> choice;

            if (choice >= 0 && choice < regionFiles.size()) {
                LoadRegions(regionFiles[choice]);
                std::cout << "Načítané regióny z: " << regionFiles[choice] << std::endl;
            }
        }
        Sleep(200);
    }

    if (GetAsyncKeyState('0') & 0x8000) {
        g_settings.showFPS = !g_settings.showFPS;
        if (!g_settings.showFPS) {
            system("cls");
            ShowMenu();
        }
        Sleep(200);
    }
    // V pre vizualizáciu
    if (GetAsyncKeyState('V') & 0x8000) {
        if (!g_lastMatches.empty()) {
            std::cout << "\nZobrazujem " << g_lastMatches.size() << " nájdených pozícií..." << std::endl;
            DrawHitVisualization();
            Sleep(2000);  // Zobraz na 2 sekundy
        }
        else {
            std::cout << "\nŽiadne zhody na vizualizáciu!" << std::endl;
        }
        Sleep(200);
    }

    Sleep(30);  // Redukuj CPU usage a zlepši responzivitu


// Počkaj na threads

    processingThread.join();
    displayThread.join();

    // Ulož posledné regióny
    SaveRegions("last_regions.txt");

    // Ulož štatistiky učenia
    SaveLearningStats();  // <-- PRIDAJ TOTO

    // Ulož konfiguráciu
    SaveConfig();  // <-- PRIDAJ AJ TOTO

    std::cout << "Program ukončený.\n";
    return 0;
}
        

// ===== DOPLNKOVÉ FUNKCIE PRE ROZŠÍRENÉ FEATURES =====

// Funkcia pre náhodné testovanie pixelov (ak je zapnuté)
float MatchTemplateRandom(const uint8_t* image, int imgStride,
    const uint8_t* tmpl, int tolerance, int pixelCount) {
    // Generuj náhodné pozície - použijeme C++11 random
    std::vector<int> indices;
    for (int i = 0; i < TEMPLATE_SIZE * TEMPLATE_SIZE; i++) {
        indices.push_back(i);
    }

    // C++11 spôsob namiesto std::random_shuffle
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(indices.begin(), indices.end(), g);

    int totalDiff = 0;
    int tested = 0;

    for (int i = 0; i < min(pixelCount, (int)indices.size()); i++) {
        int idx = indices[i];
        int y = idx / TEMPLATE_SIZE;
        int x = idx % TEMPLATE_SIZE;

        // Porovnaj BGRA hodnoty
        for (int c = 0; c < 4; c++) {
            int imgVal = image[y * imgStride + x * 4 + c];
            int tmplVal = tmpl[y * TEMPLATE_SIZE * 4 + x * 4 + c];
            totalDiff += abs(imgVal - tmplVal);
        }

        tested++;

        // Early rejection
        if (totalDiff > tolerance * tested * 4) {
            return FLT_MAX;
        }
    }

    // Extrapoluj na celú šablónu
    return (float)totalDiff * (TEMPLATE_SIZE * TEMPLATE_SIZE) / tested;
}

// Funkcia pre export štatistík do CSV
void ExportStats(const std::string& filename) {
    std::ofstream file(filename);
    if (!file) return;

    file << "Template,HitCount,AvgX,AvgY,Probability\n";

    for (int i = 0; i < g_templates.size(); i++) {
        const auto& stats = g_templateStats[i];
        float prob = (float)stats.hitCount / (g_fps * 60);  // Za minútu

        file << g_templates[i].filename << ","
            << stats.hitCount << ","
            << stats.avgPosition.x << ","
            << stats.avgPosition.y << ","
            << prob << "\n";
    }

    std::cout << "Štatistiky exportované do: " << filename << std::endl;
}

// Funkcia pre optimalizované poradie šablón podľa učenia
std::vector<int> GetOptimizedTemplateOrder() {
    std::vector<std::pair<float, int>> priorities;

    for (int i = 0; i < g_templates.size(); i++) {
        // Priorita = počet hitov * (1 / priemerný čas od posledného hitu)
        float priority = g_templateStats[i].hitCount;

        if (!g_templateStats[i].hitPositions.empty()) {
            // Bonus pre nedávno nájdené šablóny
            auto now = std::chrono::steady_clock::now();
            // Tu by sme mali mať timestamp posledného hitu
            priority *= 2.0f;  // Zatiaľ jednoduchý bonus
        }

        priorities.push_back({ priority, i });
    }

    // Zoraď podľa priority (najvyššia prvá)
    std::sort(priorities.rbegin(), priorities.rend());

    std::vector<int> order;
    for (const auto& p : priorities) {
        order.push_back(p.second);
    }

    return order;
}

// Funkcia pre adaptívne regióny na základe učenia
std::vector<SearchRegion> GetAdaptiveRegions(int templateId) {
    std::vector<SearchRegion> adaptiveRegions;

    const auto& stats = g_templateStats[templateId];
    if (stats.hitPositions.size() < 10) {
        // Málo dát, použi defaultné regióny
        return g_searchRegions;
    }

    // Vytvor región okolo priemernej pozície
    SearchRegion avgRegion;
    avgRegion.x = stats.avgPosition.x - 50;
    avgRegion.y = stats.avgPosition.y - 50;
    avgRegion.width = 100;
    avgRegion.height = 100;
    avgRegion.name = "Adaptive_" + std::to_string(templateId);
    avgRegion.active = true;

    // Uisti sa, že je v rámci obrazovky
    avgRegion.x = max(0, min(SCREEN_WIDTH - avgRegion.width, avgRegion.x));
    avgRegion.y = max(0, min(SCREEN_HEIGHT - avgRegion.height, avgRegion.y));

    adaptiveRegions.push_back(avgRegion);

    // Pridaj aj expandovaný región pre istotu
    SearchRegion expandedRegion = avgRegion;
    expandedRegion.x -= 50;
    expandedRegion.y -= 50;
    expandedRegion.width += 100;
    expandedRegion.height += 100;
    expandedRegion.x = max(0, expandedRegion.x);
    expandedRegion.y = max(0, expandedRegion.y);
    expandedRegion.name += "_expanded";

    adaptiveRegions.push_back(expandedRegion);

    return adaptiveRegions;
}

// Funkcia pre vizualizáciu hitov (kreslí bodky kde našiel šablóny)
void DrawHitVisualization() {
    HDC screenDC = GetDC(NULL);

    for (const auto& match : g_lastMatches) {
        // Kresli červený krížik na pozícii
        HPEN redPen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
        HPEN oldPen = (HPEN)SelectObject(screenDC, redPen);

        MoveToEx(screenDC, match.x - 5, match.y, NULL);
        LineTo(screenDC, match.x + 5, match.y);
        MoveToEx(screenDC, match.x, match.y - 5, NULL);
        LineTo(screenDC, match.x, match.y + 5);

        SelectObject(screenDC, oldPen);
        DeleteObject(redPen);

        // Zobraz ID šablóny
        std::string text = std::to_string(match.templateId);
        TextOutA(screenDC, match.x + 7, match.y - 7, text.c_str(), text.length());
    }

    ReleaseDC(NULL, screenDC);
}

// ===== KONFIGURAČNÝ SÚBOR =====

//void SaveConfig(const std::string& filename = "config.ini") {
//    std::ofstream file(filename);
//    if (!file) return;
//
//    file << "[Settings]\n";
//    file << "ClickOnMatch=" << g_settings.clickOnMatch << "\n";
//    file << "DoubleClick=" << g_settings.doubleClick << "\n";
//    file << "Tolerance=" << g_settings.tolerance << "\n";
//    file << "EarlyPixelCount=" << g_settings.earlyPixelCount << "\n";
//    file << "RandomPixelTest=" << g_settings.randomPixelTest << "\n";
//    file << "UseAVX2=" << g_settings.useAVX2 << "\n";
//    file << "ShowFPS=" << g_settings.showFPS << "\n";
//    file << "EnableLearning=" << g_settings.enableLearning << "\n";
//}

//void LoadConfig(const std::string& filename = "config.ini") {
//    std::ifstream file(filename);
//    if (!file) return;
//
//    std::string line;
//    while (std::getline(file, line)) {
//        if (line.find("=") != std::string::npos) {
//            std::string key = line.substr(0, line.find("="));
//            std::string value = line.substr(line.find("=") + 1);
//
//            if (key == "ClickOnMatch") g_settings.clickOnMatch = std::stoi(value);
//            else if (key == "DoubleClick") g_settings.doubleClick = std::stoi(value);
//            else if (key == "Tolerance") g_settings.tolerance = std::stoi(value);
//            else if (key == "EarlyPixelCount") g_settings.earlyPixelCount = std::stoi(value);
//            else if (key == "RandomPixelTest") g_settings.randomPixelTest = std::stoi(value);
//            else if (key == "UseAVX2") g_settings.useAVX2 = std::stoi(value);
//            else if (key == "ShowFPS") g_settings.showFPS = std::stoi(value);
//            else if (key == "EnableLearning") g_settings.enableLearning = std::stoi(value);
//        }
//    }
//}

