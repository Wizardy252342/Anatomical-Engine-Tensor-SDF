#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <atomic>

// Подключаем наше единое независимое C++17 SDK ядро!
#include "AnatomicalCore.hpp"

using Anatomical::Vec3;
using Anatomical::Vec4;
using Anatomical::ESNPacket256;

const unsigned int SCR_WIDTH = 1280;
const unsigned int SCR_HEIGHT = 720;
const unsigned int COARSE_WIDTH = 320;
const unsigned int COARSE_HEIGHT = 180;

// Описание мира через стандартный интерфейс ядра ISpatialWorld
class SandboxPlanetWorld : public Anatomical::ISpatialWorld {
public:
    float EvaluateBaseSDF(const Vec3& p) const override {
        float sdfPlanet = Anatomical::SDF::Sphere(p, { 0.0f, -15000000.0f, 0.0f }, 15000000.0f);
        float gyroid = Anatomical::SDF::Gyroid(p, 0.0006f) - 420.0f;
        float sx = p.x - std::sin(p.z * 0.0002f) * 1800.0f;
        float sy = p.y - std::cos(p.z * 0.00015f) * 1200.0f;
        float mainShaft = std::sqrt(sx * sx + sy * sy) - 950.0f;
        float sdfCaves = std::min(mainShaft, gyroid);
        float zBounds = std::max(-p.z, p.z - 105000.0f);
        sdfCaves = std::max(sdfCaves, zBounds);
        return std::max(sdfPlanet, -sdfCaves);
    }
};

// Глобальные экземпляры единого ядра: бесшовная хеш-сетка + роутер Меркла
Anatomical::SpatialHashGrid g_HashGrid(500.0f);
Anatomical::HierarchicalHashRouter g_MerkleRouter;
SandboxPlanetWorld g_World;
uint64_t g_CurrentMerkleHash = 0ULL;

// Состояние симуляции
bool waveFrontActive = false;
Vec3 waveOrigin = { 0.0f, 450.0f, 1000.0f };
float waveRadius = 0.0f;
float waveFrontSpeed = 45000.0f;

Vec3 camPos = { 0.0f, 450.0f, 1000.0f };
Vec3 camVel = { 0.0f, 0.0f, 0.0f };
bool physicsCollisionEnabled = true;
bool cityModeEnabled = true;

float yaw = 0.0f, pitch = 0.0f;
float lastX = SCR_WIDTH / 2.0f, lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;
float deltaTime = 0.0f, lastFrame = 0.0f;
float globalTime = 0.0f;

// --- Встроенный процедурный синтезатор звука ---
std::atomic<float> audioExplosionEnv{ 0.0f };
std::atomic<float> audioWaveEnv{ 0.0f };
std::atomic<float> audioEngineThrust{ 0.0f };
std::atomic<float> audioCavernDepth{ 0.0f };

void PlayProceduralSoundBuffer(HWAVEOUT hWaveOut, WAVEHDR* hdr, std::vector<int16_t>& buf, double& phase1, double& phase2) {
    const double sampleRate = 22050.0;
    float expEnv = audioExplosionEnv.load();
    float wavEnv = audioWaveEnv.load();
    float thrust = audioEngineThrust.load();
    float depth = audioCavernDepth.load();

    for (size_t i = 0; i < buf.size(); i++) {
        double engineFreq = 48.0 + thrust * 65.0 + std::min(depth * 0.25, 35.0);
        phase1 += (2.0 * 3.14159265 * engineFreq) / sampleRate;
        double engineHum = std::sin(phase1) * (0.14 + thrust * 0.18);

        double explosionSound = 0.0;
        if (expEnv > 0.001f) {
            double boomFreq = 32.0 + expEnv * 95.0;
            phase2 += (2.0 * 3.14159265 * boomFreq) / sampleRate;
            double whiteNoise = (static_cast<double>(std::rand() % 2000) / 1000.0) - 1.0;
            explosionSound = (std::sin(phase2) * 0.7 + whiteNoise * 0.35) * expEnv * 0.65;
            expEnv *= 0.99965f;
        }

        double waveSound = 0.0;
        if (wavEnv > 0.001f) {
            double mod = std::sin(phase1 * 2.5) * std::cos(phase2 * 0.5);
            waveSound = mod * wavEnv * 0.45;
            wavEnv *= 0.99982f;
        }

        double mixed = std::max(-0.95, std::min(0.95, engineHum + explosionSound + waveSound));
        buf.at(i) = static_cast<int16_t>(mixed * 32000.0);
    }

    audioExplosionEnv.store(expEnv);
    audioWaveEnv.store(wavEnv);
    waveOutWrite(hWaveOut, hdr, sizeof(WAVEHDR));
}

// --- Сетевой модуль UDP (256-байтные пакеты ядра) ---
SOCKET udpSocket = INVALID_SOCKET;
uint32_t localNodeId = 0;
uint16_t boundPort = 0;
int networkPacketsSent = 0;
int networkPacketsRecv = 0;

void InitNetworkUDP() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) return;

    u_long nonBlocking = 1;
    ioctlsocket(udpSocket, FIONBIO, &nonBlocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(7777);

    if (bind(udpSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        boundPort = 7777;
    } else {
        addr.sin_port = htons(7778);
        if (bind(udpSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            boundPort = 7778;
        }
    }
    localNodeId = static_cast<uint32_t>(GetCurrentProcessId());
}

void SendESNPacket256(uint32_t opType, Vec3 pos, float tempK, uint32_t seedVal = 0) {
    if (udpSocket == INVALID_SOCKET) return;

    ESNPacket256 pkt{};
    pkt.senderId = localNodeId;
    pkt.opType = opType;
    pkt.seed = seedVal;
    pkt.posX = pos.x;
    pkt.posY = pos.y;
    pkt.posZ = pos.z;
    pkt.temperatureK = tempK;

    uint16_t targetPort = (boundPort == 7777) ? 7778 : 7777;
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons(targetPort);

    sendto(
        udpSocket,
        reinterpret_cast<const char*>(&pkt),
        sizeof(ESNPacket256),
        0,
        reinterpret_cast<sockaddr*>(&dest),
        sizeof(dest)
    );
    networkPacketsSent++;
}

void SpawnCraterUnified(Vec3 pos, float spawnTime, bool playSound = true) {
    Anatomical::PerturbationOperator op;
    op.position = pos;
    op.timestamp = spawnTime;
    g_HashGrid.InsertOperator(op); // Бесшовная вставка с ореолом (Halo Insertion)
    g_MerkleRouter.RebuildFromGrid(g_HashGrid);
    g_CurrentMerkleHash = g_MerkleRouter.GetGlobalRootHash();

    if (playSound) {
        float dist = (pos - camPos).Length();
        float atten = 1.0f / (1.0f + dist * 0.0004f);
        audioExplosionEnv.store(std::min(1.0f, audioExplosionEnv.load() + atten));
    }
}

void TriggerOrbitalBombardment(float currentTime, uint32_t seedVal) {
    std::srand(seedVal);
    for (int i = 0; i < 250; i++) {
        float zPos = 1500.0f + static_cast<float>(i) * 410.0f;
        float cx = std::sin(zPos * 0.0002f) * 1800.0f;
        float cy = cityModeEnabled ? 120.0f : (std::cos(zPos * 0.00015f) * 1200.0f - 850.0f);
        float offsetX = static_cast<float>((std::rand() % 1200) - 600);
        Anatomical::PerturbationOperator op;
        op.position = { cx + offsetX, cy, zPos };
        op.timestamp = currentTime;
        g_HashGrid.InsertOperator(op);
    }
    g_MerkleRouter.RebuildFromGrid(g_HashGrid);
    g_CurrentMerkleHash = g_MerkleRouter.GetGlobalRootHash();
    audioExplosionEnv.store(1.0f);
}

void PollNetworkUDP() {
    if (udpSocket == INVALID_SOCKET) return;
    ESNPacket256 pkt{};
    sockaddr_in from{};
    int fromLen = sizeof(from);

    while (true) {
        int bytes = recvfrom(
            udpSocket,
            reinterpret_cast<char*>(&pkt),
            sizeof(ESNPacket256),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen
        );
        if (bytes != sizeof(ESNPacket256)) break;
        if (!pkt.IsValid() || pkt.senderId == localNodeId) continue;

        networkPacketsRecv++;
        Vec3 remotePos = { pkt.posX, pkt.posY, pkt.posZ };
        if (pkt.opType == 1) {
            SpawnCraterUnified(remotePos, globalTime, true);
        } else if (pkt.opType == 2) {
            TriggerOrbitalBombardment(globalTime, pkt.seed);
        } else if (pkt.opType == 3) {
            waveFrontActive = true;
            waveOrigin = remotePos;
            waveRadius = 50.0f;
            audioWaveEnv.store(1.0f);
        }
    }
}

const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
out vec2 uv;
void main() {
    uv = aPos;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* coarseFragmentShaderSource = R"(
#version 330 core
out float outStartDepth;
in vec2 uv;

uniform vec2 uResolution;
uniform vec3 uCamPos;
uniform vec3 uCamForward;
uniform vec3 uCamRight;
uniform vec3 uCamUp;

float FastMacroSDF(vec3 p) {
    float sdfPlanet = length(p + vec3(0.0, 15000000.0, 0.0)) - 15000000.0;
    vec3 g = p * 0.0006;
    float gyroid = abs(dot(sin(g), cos(g.yzx))) / 0.0006;
    vec2 shaftOffset = vec2(sin(p.z * 0.0002) * 1800.0, cos(p.z * 0.00015) * 1200.0);
    float mainShaft = length(p.xy - shaftOffset) - 950.0;
    float sdfCaves = min(mainShaft, gyroid - 420.0);
    float zBounds = max(-p.z, p.z - 105000.0);
    return max(sdfPlanet, -max(sdfCaves, zBounds));
}

void main() {
    vec2 screenUV = uv * vec2(uResolution.x / uResolution.y, 1.0);
    vec3 rayDir = normalize(uCamForward + screenUV.x * uCamRight + screenUV.y * uCamUp);
    float t = 0.0;
    for (int i = 0; i < 48; i++) {
        float d = FastMacroSDF(uCamPos + rayDir * t);
        if (d < 260.0 || t > 120000.0) break;
        t += (d - 240.0) * 1.1;
    }
    outStartDepth = max(0.0, t - 260.0);
}
)";
const char* mainFragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
in vec2 uv;

uniform vec2 uResolution;
uniform vec3 uCamPos;
uniform vec3 uCamForward;
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform samplerBuffer uHashGrid;
uniform sampler2D uCoarseDepth;
uniform float uTime;
uniform vec3 uWaveOrigin;
uniform float uWaveRadius;
uniform int uWaveActive;
uniform int uCityMode;

const mat3 rotSO3 = mat3(
     0.36,  0.48, -0.80,
    -0.80,  0.60,  0.00,
     0.48,  0.64,  0.60
);

float Hash21(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

float EscherTensorNoise(vec3 p) {
    vec3 q = p * 0.032;
    float n1 = sin(q.x) * cos(q.y) * sin(q.z);
    q = rotSO3 * q * 2.13;
    float n2 = sin(q.x) * cos(q.y) * sin(q.z) * 0.45;
    return n1 + n2;
}

vec3 BlackbodyRadiation(float tempK) {
    float t = clamp((tempK - 700.0) / 2500.0, 0.0, 1.0);
    vec3 cold = vec3(0.45, 0.04, 0.01);
    vec3 warm = vec3(1.00, 0.38, 0.04);
    vec3 hot  = vec3(1.00, 0.96, 0.85);
    if (t < 0.5) return mix(cold, warm, t * 2.0);
    return mix(warm, hot, (t - 0.5) * 2.0);
}

uint ComputeSpatialHashGPU(ivec3 cell) {
    uvec3 u = uvec3(cell + ivec3(100000));
    uint h = (u.x * 73856093u) ^ (u.y * 19349663u) ^ (u.z * 83492791u);
    return h % 16384u;
}

float EvalSpatialGridCraters(vec3 p, out float outTempK) {
    outTempK = 0.0;
    float totalDelta = 0.0;
    ivec3 cell = ivec3(floor(p / 500.0));
    uint bucket = ComputeSpatialHashGPU(cell);
    int baseIdx = int(bucket) * 4;

    for (int i = 0; i < 4; i++) {
        vec4 c = texelFetch(uHashGrid, baseIdx + i);
        if (c.w > 0.5) {
            vec3 diff = (p - c.xyz) * vec3(1.15, 0.92, 1.08);
            float dist = length(diff);
            if (dist < 480.0) {
                float age = max(0.0, uTime - (c.w - 1.0));
                float cooling = exp(-age * 0.08);
                float rNorm = dist / 115.0;
                float bowl = 230.0 * exp(-0.5 * rNorm * rNorm);
                float rim = -45.0 * exp(-0.5 * pow((dist - 240.0) / 55.0, 2.0));
                totalDelta += (bowl + rim);
                outTempK += (2800.0 * cooling + 650.0) * exp(-0.5 * pow(dist / 150.0, 2.0));
            }
        }
    }
    return totalDelta;
}

// Сверхбыстрый SDF геометрии города (без расчета окон внутри цикла луча!)
float ArcCorpCityFastSDF(vec3 p) {
    float blockSize = 220.0;
    vec2 cellID = floor(p.xz / blockSize);
    vec2 localXZ = mod(p.xz, blockSize) - blockSize * 0.5;
    float rnd = Hash21(cellID);
    
    // Шахты-входы в 105-км подземные пещеры
    if (rnd > 0.91 && p.z > 0.0 && p.z < 105000.0) return 150.0;
    
    float rnd2 = Hash21(cellID + vec2(17.3, 41.9));
    float bHeight = 120.0 + pow(rnd, 1.8) * 680.0;
    vec2 bHalfSize = vec2(58.0 + rnd2 * 28.0, 58.0 + rnd * 28.0);
    
    vec3 q = abs(vec3(localXZ.x, p.y - bHeight * 0.5, localXZ.y)) - vec3(bHalfSize.x, bHeight * 0.5, bHalfSize.y);
    float towerSDF = length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
    return min(towerSDF, p.y);
}

// Расчет неоновых окон, маяков и трафика спидеров (вызывается ОДИН РАЗ при попадании луча!)
vec3 EvaluateCityGlowAtHit(vec3 p) {
    float blockSize = 220.0;
    vec2 cellID = floor(p.xz / blockSize);
    vec2 localXZ = mod(p.xz, blockSize) - blockSize * 0.5;
    float rnd = Hash21(cellID);
    float rnd2 = Hash21(cellID + vec2(17.3, 41.9));
    float bHeight = 120.0 + pow(rnd, 1.8) * 680.0;
    vec2 bHalfSize = vec2(58.0 + rnd2 * 28.0, 58.0 + rnd * 28.0);
    
    vec3 glow = vec3(0.0);
    if (p.y > 15.0 && p.y < bHeight - 8.0) {
        float winY = fract(p.y * 0.14);
        float winX = fract((localXZ.x + localXZ.y) * 0.12);
        float winMask = step(0.35, winY) * step(0.3, winX);
        float lit = step(0.45, Hash21(floor(vec2((localXZ.x + localXZ.y) * 0.12, p.y * 0.14)) + cellID));
        vec3 winColor = (rnd2 > 0.5) ? vec3(1.0, 0.65, 0.25) : vec3(0.25, 0.85, 1.0);
        glow += winColor * winMask * lit * 1.45;
    }
    if (p.y > bHeight - 5.0 && length(localXZ) < 16.0) {
        float pulse = 0.5 + 0.5 * sin(uTime * 5.0 + rnd * 10.0);
        glow += vec3(1.0, 0.12, 0.08) * pulse * 2.5;
    }
    float laneH = abs(p.y - (140.0 + floor(rnd2 * 3.0) * 90.0));
    if (laneH < 12.0) {
        float t1 = pow(max(0.0, sin(p.z * 0.03 - uTime * 18.0 + cellID.x)), 16.0);
        float t2 = pow(max(0.0, sin(p.x * 0.03 + uTime * 22.0 + cellID.y)), 16.0);
        glow += vec3(1.0, 0.2, 0.1) * t1 * 2.0 + vec3(0.2, 0.8, 1.0) * t2 * 2.0;
    }
    return glow;
}

float LiquidMetalOceanSDF(vec3 p) {
    float wave1 = sin(p.x * 0.025 + uTime * 1.8) * cos(p.z * 0.025 - uTime * 1.3) * 4.5;
    float wave2 = sin((p.x + p.z) * 0.07 - uTime * 3.1) * 1.4;
    return (p.y + 620.0) - (wave1 + wave2);
}

float GetDistanceAndPhysics(vec3 p, out vec3 outMagField, out float outTempK, out int outMatID) {
    float sdfPlanet = length(p + vec3(0.0, 15000000.0, 0.0)) - 15000000.0;
    vec3 g = p * 0.0006; 
    float gyroid = abs(dot(sin(g), cos(g.yzx))) / 0.0006;
    vec2 shaftOffset = vec2(sin(p.z * 0.0002) * 1800.0, cos(p.z * 0.00015) * 1200.0);
    float mainShaft = length(p.xy - shaftOffset) - 950.0;
    float sdfCaves = min(mainShaft, gyroid - 420.0);
    float zBounds = max(-p.z, p.z - 105000.0);
    sdfCaves = max(sdfCaves, zBounds);
    
    float baseSDF = max(sdfPlanet, -sdfCaves);
    float damping = exp(-0.00008 * baseSDF * baseSDF);
    float microDetails = EscherTensorNoise(p) * 36.0 * damping;
    
    float craterTemp = 0.0;
    float deltaDestruction = EvalSpatialGridCraters(p, craterTemp);
    
    float geoTemp = clamp((p.z / 105000.0) * 950.0, 0.0, 950.0);
    float waveTemp = 0.0;
    if (uWaveActive == 1) {
        float distWave = abs(length(p - uWaveOrigin) - uWaveRadius);
        if (distWave < 1800.0) {
            float interference = 0.5 + 0.5 * sin(p.x * 0.012 + p.z * 0.012) * cos(p.y * 0.012);
            waveTemp = 3200.0 * exp(-(distWave * distWave) / (2.0 * 450.0 * 450.0)) * interference;
            deltaDestruction += 55.0 * exp(-(distWave * distWave) / (2.0 * 350.0 * 350.0));
        }
    }
    
    outTempK = craterTemp + geoTemp + waveTemp;
    outMagField = vec3(
        sin(p.y * 0.004 + p.z * 0.001 + uTime * 0.4),
        cos(p.x * 0.004 - p.z * 0.001),
        sin(p.z * 0.002)
    );
    
    if (uCityMode == 1 && p.y > -20.0) {
        outMatID = 3;
        return ArcCorpCityFastSDF(p) + deltaDestruction;
    }
    
    float rockSDF = baseSDF + microDetails + deltaDestruction;
    float oceanSDF = LiquidMetalOceanSDF(p);
    if (oceanSDF < rockSDF && p.z > 0.0 && p.z < 105000.0) {
        outMatID = 2;
        return oceanSDF;
    }
    outMatID = 1;
    return rockSDF;
}

vec3 CalculateNormal(vec3 p) {
    vec2 e = vec2(0.35, 0.0);
    vec3 m; float temp; int mat;
    float d = GetDistanceAndPhysics(p, m, temp, mat);
    return normalize(vec3(
        GetDistanceAndPhysics(p + e.xyy, m, temp, mat) - d,
        GetDistanceAndPhysics(p + e.yxy, m, temp, mat) - d,
        GetDistanceAndPhysics(p + e.yyx, m, temp, mat) - d
    ));
}

float CalculateAO(vec3 p, vec3 n) {
    float occ = 0.0, sca = 1.0;
    vec3 m; float temp; int mat;
    for (int i = 1; i <= 3; i++) {
        float h = 10.0 * float(i);
        float d = GetDistanceAndPhysics(p + h * n, m, temp, mat);
        occ += (h - d) * sca;
        sca *= 0.6;
    }
    return clamp(1.0 - 0.025 * occ, 0.2, 1.0);
}

vec3 TraceSecondaryReflection(vec3 origin, vec3 reflDir) {
    float t = 5.0;
    vec3 p = origin, magField = vec3(0.0);
    float tempK = 0.0, d = 1000.0;
    int matID = 1;
    for (int i = 0; i < 24; i++) {
        p = origin + reflDir * t;
        d = GetDistanceAndPhysics(p, magField, tempK, matID);
        t += d * 1.15;
        if (d < 1.5 || t > 35000.0) break;
    }
    vec3 bg = vec3(0.02, 0.05, 0.11);
    if (d < 1.5) {
        float fog = exp(-t * 0.00006);
        float magLine = pow(abs(sin(magField.x * 3.0 + magField.y * 3.0)), 10.0);
        vec3 rock = vec3(0.28, 0.30, 0.36);
        vec3 mag = vec3(0.0, 0.72, 1.0) * magLine;
        vec3 glow = (tempK > 700.0) ? BlackbodyRadiation(tempK) * 1.2 : vec3(0.0);
        return mix(bg, rock + mag + glow, fog);
    }
    return bg;
}

// Векторный тактический HUD кокпита
vec3 RenderTacticalHUD(vec3 col, vec2 screenUV) {
    vec3 hudColor = (uCityMode == 1) ? vec3(0.15, 0.95, 1.0) : vec3(1.0, 0.55, 0.15);
    float distCenter = length(screenUV);
    
    // Кольцо прицела и центральная точка
    if (abs(distCenter - 0.024) < 0.0012 || distCenter < 0.0025) {
        col = mix(col, hudColor, 0.75);
    }
    // Боковые шкалы высоты/глубины (Pitch Ladder)
    if (abs(abs(screenUV.x) - 0.16) < 0.0012 && abs(screenUV.y) < 0.18) {
        float ticks = step(0.85, fract((screenUV.y + uCamPos.y * 0.0005) * 18.0));
        col = mix(col, hudColor, 0.35 + 0.45 * ticks);
    }
    return col;
}

void main() {
    vec2 screenUV = uv * vec2(uResolution.x / uResolution.y, 1.0);
    vec3 rayDir = normalize(
        uCamForward + screenUV.x * uCamRight + screenUV.y * uCamUp
    );
    
    vec2 texUV = uv * 0.5 + 0.5;
    float t = (uCityMode == 1) ? 0.0 : texture(uCoarseDepth, texUV).r;
    
    // Быстрое отсечение высокого неба в режиме города (ускорение до 100+ FPS)
    if (uCityMode == 1 && uCamPos.y > 820.0 && rayDir.y > 0.0) {
        t = 120000.0;
    } else if (uCityMode == 1 && uCamPos.y > 820.0 && rayDir.y < 0.0) {
        t = max(0.0, (810.0 - uCamPos.y) / rayDir.y);
    }
        if (uCityMode == 0 && rayDir.y < -0.02 && uCamPos.y > -620.0) {
        float tOcean = (-620.0 - uCamPos.y) / rayDir.y;
        t = min(t, max(0.0, tOcean - 50.0));
    }
    
    vec3 p = uCamPos, magField = vec3(0.0);
    float tempK = 0.0, d = 1000.0;
    int matID = 1;
    vec3 volumetricAccum = vec3(0.0);
    float transmittance = 1.0;
    
    if (t < 119500.0) {
        for (int i = 0; i < 55; i++) {
            p = uCamPos + rayDir * t;
            d = GetDistanceAndPhysics(p, magField, tempK, matID);
            
            float stepLen = max(1.5, d * 0.90);
            float gasDensity = (uCityMode == 1) ? 0.000022 : 0.000035;
            if (tempK > 800.0) {
                float glowScale = clamp((tempK - 800.0) / 1500.0, 0.0, 1.5);
                vec3 scatterCol = BlackbodyRadiation(tempK) * 0.00045 * glowScale;
                volumetricAccum += transmittance * scatterCol * min(stepLen, 400.0);
            }
            transmittance *= exp(-gasDensity * min(stepLen, 500.0));
            
            t += stepLen;
            if (d < 0.25 || t > 120000.0) break;
        }
    }
    
    vec3 fogColor = (uCityMode == 1) 
        ? vec3(0.04, 0.05, 0.11) 
        : vec3(0.03, 0.06, 0.12);
    vec3 surfaceCol = fogColor;
    
    if (d < 0.25) {
        vec3 normal = CalculateNormal(p);
        float fog = exp(-t * 0.000045);
        
        if (matID == 2) {
            vec3 reflDir = reflect(rayDir, normal);
            vec3 reflectedColor = TraceSecondaryReflection(
                p + normal * 2.0, reflDir
            );
            float fresnel = pow(1.0 - max(0.0, dot(-rayDir, normal)), 4.0);
            vec3 metalBase = vec3(0.08, 0.12, 0.18);
            vec3 finalMetal = mix(
                metalBase, reflectedColor * 1.35, 0.35 + 0.65 * fresnel
            );
            surfaceCol = mix(fogColor, finalMetal, fog);
        } else if (matID == 3) {
            float ao = CalculateAO(p, normal);
            vec3 sunDir = normalize(vec3(0.4, 0.7, 0.5));
            float diff = max(0.15, dot(normal, sunDir));
            vec3 steelCol = vec3(0.16, 0.19, 0.25) * diff * ao;
            vec3 cityGlow = EvaluateCityGlowAtHit(p);
            vec3 thermalGlow = (tempK > 700.0) 
                ? BlackbodyRadiation(tempK) * 1.5 
                : vec3(0.0);
            surfaceCol = mix(fogColor, steelCol + cityGlow + thermalGlow, fog);
        } else {
            float ao = CalculateAO(p, normal);
            vec3 lightDir = normalize((uCamPos + uCamForward * 800.0) - p);
            float diff = max(0.18, dot(normal, lightDir));
            float magLine = pow(
                abs(sin(magField.x * 3.0 + magField.y * 3.0)), 10.0
            );
            vec3 rockCol = vec3(0.44, 0.42, 0.47) * diff * ao;
            vec3 magCol = vec3(0.0, 0.72, 1.0) * magLine;
            vec3 thermalGlow = vec3(0.0);
            if (tempK > 700.0) {
                float intensity = clamp((tempK - 700.0) / 1400.0, 0.0, 1.8);
                thermalGlow = BlackbodyRadiation(tempK) * intensity;
            }
            surfaceCol = mix(fogColor, rockCol + magCol + thermalGlow, fog);
        }
    }
    
    vec3 col = surfaceCol * transmittance + volumetricAccum;
    col = RenderTacticalHUD(col, screenUV);
    FragColor = vec4(col, 1.0);
}
)";

void mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);
    if (firstMouse) { lastX = xpos; lastY = ypos; firstMouse = false; }
    yaw += (xpos - lastX) * 0.15f;
    pitch += (lastY - ypos) * 0.15f;
    lastX = xpos; lastY = ypos;
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
}

void mouse_button_callback(
    GLFWwindow* window, int button, int action, int mods
) {
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        float radYaw = yaw * 3.14159f / 180.0f;
        float radPitch = pitch * 3.14159f / 180.0f;
        Vec3 fwd = {
            std::sin(radYaw) * std::cos(radPitch),
            std::sin(radPitch),
            std::cos(radYaw) * std::cos(radPitch)
        };
        Vec3 target = camPos + fwd * 1200.0f;
        SpawnCraterUnified(target, globalTime, true);
        SendESNPacket256(1, target, 2800.0f);
    }
}

unsigned int CompileShaderProgram(const char* vsSrc, const char* fsSrc) {
    unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSrc, NULL);
    glCompileShader(vs);

    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSrc, NULL);
    glCompileShader(fs);

    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    return prog;
}

int main() {
    InitNetworkUDP();

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 22050;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = 44100;

    HWAVEOUT hWaveOut = NULL;
    bool audioReady = (
        waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR
    );

    std::vector<int16_t> audioBuf0(1024, 0);
    std::vector<int16_t> audioBuf1(1024, 0);
    WAVEHDR hdr0{}, hdr1{};
    double phase1 = 0.0, phase2 = 0.0;

    if (audioReady) {
        hdr0.lpData = reinterpret_cast<LPSTR>(audioBuf0.data());
        hdr0.dwBufferLength = static_cast<DWORD>(audioBuf0.size() * sizeof(int16_t));
        waveOutPrepareHeader(hWaveOut, &hdr0, sizeof(WAVEHDR));

        hdr1.lpData = reinterpret_cast<LPSTR>(audioBuf1.data());
        hdr1.dwBufferLength = static_cast<DWORD>(audioBuf1.size() * sizeof(int16_t));
        waveOutPrepareHeader(hWaveOut, &hdr1, sizeof(WAVEHDR));

        PlayProceduralSoundBuffer(hWaveOut, &hdr0, audioBuf0, phase1, phase2);
        PlayProceduralSoundBuffer(hWaveOut, &hdr1, audioBuf1, phase1, phase2);
    }

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(
        SCR_WIDTH, SCR_HEIGHT, "Anatomical Engine v8.0 (Unified Core)", NULL, NULL
    );
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    unsigned int coarseProgram = CompileShaderProgram(
        vertexShaderSource, coarseFragmentShaderSource
    );
    unsigned int mainProgram = CompileShaderProgram(
        vertexShaderSource, mainFragmentShaderSource
    );

    unsigned int coarseFBO, coarseDepthTex;
    glGenFramebuffers(1, &coarseFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, coarseFBO);

    glGenTextures(1, &coarseDepthTex);
    glBindTexture(GL_TEXTURE_2D, coarseDepthTex);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_R32F, 
        COARSE_WIDTH, COARSE_HEIGHT, 0, GL_RED, GL_FLOAT, NULL
    );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, coarseDepthTex, 0
    );
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    unsigned int hashTBO, hashTex;
    glGenBuffers(1, &hashTBO);
    glBindBuffer(GL_TEXTURE_BUFFER, hashTBO);
    glBufferData(
        GL_TEXTURE_BUFFER, 
        g_HashGrid.GetBufferByteSize(), 
        g_HashGrid.GetRawBuffer(), 
        GL_DYNAMIC_DRAW
    );

    glGenTextures(1, &hashTex);
    glBindTexture(GL_TEXTURE_BUFFER, hashTex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, hashTBO);

    std::vector<float> quadVertices = { 
        -1.0f,  1.0f, -1.0f, -1.0f,  1.0f, -1.0f, 
        -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f 
    };
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO); 
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO); 
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(
        GL_ARRAY_BUFFER, 
        quadVertices.size() * sizeof(float), 
        quadVertices.data(), 
        GL_STATIC_DRAW
    );
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    std::vector<char> titleBuffer(256);
    bool cKeyPressed = false, bKeyPressed = false;
    bool xKeyPressed = false, mKeyPressed = false;

    while (!glfwWindowShouldClose(window)) {
        globalTime = static_cast<float>(glfwGetTime());
        deltaTime = std::min(globalTime - lastFrame, 0.05f);
        lastFrame = globalTime;

        PollNetworkUDP();

                if (audioReady) {
            if (hdr0.dwFlags & WHDR_DONE) {
                PlayProceduralSoundBuffer(hWaveOut, &hdr0, audioBuf0, phase1, phase2);
            }
            if (hdr1.dwFlags & WHDR_DONE) {
                PlayProceduralSoundBuffer(hWaveOut, &hdr1, audioBuf1, phase1, phase2);
            }
        }

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, true);
        }
        if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS && !cKeyPressed) {
            physicsCollisionEnabled = !physicsCollisionEnabled;
            cKeyPressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_C) == GLFW_RELEASE) cKeyPressed = false;

        if (glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS && !mKeyPressed) {
            cityModeEnabled = !cityModeEnabled;
            mKeyPressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_M) == GLFW_RELEASE) mKeyPressed = false;

        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS && !bKeyPressed) {
            uint32_t seedVal = static_cast<uint32_t>(globalTime * 1000.0f);
            TriggerOrbitalBombardment(globalTime, seedVal);
            SendESNPacket256(2, camPos, 3000.0f, seedVal);
            bKeyPressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_RELEASE) bKeyPressed = false;

        if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS && !xKeyPressed) {
            waveFrontActive = true;
            waveOrigin = camPos;
            waveRadius = 50.0f;
            audioWaveEnv.store(1.0f);
            SendESNPacket256(3, camPos, 3200.0f);
            xKeyPressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_X) == GLFW_RELEASE) xKeyPressed = false;

        if (waveFrontActive) {
            waveRadius += waveFrontSpeed * deltaTime;
            if (waveRadius > 108000.0f) waveFrontActive = false;
        }

        if (g_HashGrid.IsDirty()) {
            glBindBuffer(GL_TEXTURE_BUFFER, hashTBO);
            glBufferSubData(
                GL_TEXTURE_BUFFER, 0, 
                g_HashGrid.GetBufferByteSize(), 
                g_HashGrid.GetRawBuffer()
            );
            g_HashGrid.ClearDirtyFlag();
        }

        float radYaw = yaw * 3.14159f / 180.0f;
        float radPitch = pitch * 3.14159f / 180.0f;
        Vec3 rawFwd = {
            std::sin(radYaw) * std::cos(radPitch),
            std::sin(radPitch),
            std::cos(radYaw) * std::cos(radPitch)
        };
        Vec3 fwd = rawFwd.Normalized();
        Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
        Vec3 right = fwd.Cross(worldUp).Normalized();
        Vec3 up = right.Cross(fwd).Normalized();

        bool isBoost = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
        float thrust = isBoost ? 25000.0f : 3500.0f;
        Vec3 accel = { 0.0f, 0.0f, 0.0f };

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) accel = accel + fwd * thrust;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) accel = accel - fwd * thrust;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) accel = accel - right * thrust;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) accel = accel + right * thrust;
        if (physicsCollisionEnabled && !cityModeEnabled) accel.y -= 180.0f;

        camVel = (camVel + accel * deltaTime) * 0.94f;
        camPos = camPos + camVel * deltaTime;

        audioEngineThrust.store(isBoost ? 1.0f : 0.15f);
        audioCavernDepth.store(std::max(0.0f, camPos.z / 1000.0f));

        if (physicsCollisionEnabled && !cityModeEnabled) {
            Anatomical::FieldPhysicsSolver::ResolveSphereCollision(
                g_World, g_HashGrid, camPos, camVel, 25.0f, 0.3f
            );
        }

        int fps = (deltaTime > 0.0f) ? static_cast<int>(1.0f / deltaTime) : 0;
        snprintf(
            titleBuffer.data(), 
            titleBuffer.size(),
            "Anatomical v8.0 | Mode:%s [M] | Hash:%llx | Craters:%d | FPS:%d",
            cityModeEnabled ? "CITY" : "CAVES",
            static_cast<unsigned long long>(g_CurrentMerkleHash),
            g_HashGrid.GetActiveCount(),
            fps
        );
        glfwSetWindowTitle(window, titleBuffer.data());

        glBindFramebuffer(GL_FRAMEBUFFER, coarseFBO);
        glViewport(0, 0, COARSE_WIDTH, COARSE_HEIGHT);
        glUseProgram(coarseProgram);
        glUniform2f(
            glGetUniformLocation(coarseProgram, "uResolution"), 
            (float)COARSE_WIDTH, (float)COARSE_HEIGHT
        );
        glUniform3f(glGetUniformLocation(coarseProgram, "uCamPos"), camPos.x, camPos.y, camPos.z);
        glUniform3f(glGetUniformLocation(coarseProgram, "uCamForward"), fwd.x, fwd.y, fwd.z);
        glUniform3f(glGetUniformLocation(coarseProgram, "uCamRight"), right.x, right.y, right.z);
        glUniform3f(glGetUniformLocation(coarseProgram, "uCamUp"), up.x, up.y, up.z);
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        int fbWidth, fbHeight;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);

        glUseProgram(mainProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_BUFFER, hashTex);
        glUniform1i(glGetUniformLocation(mainProgram, "uHashGrid"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, coarseDepthTex);
        glUniform1i(glGetUniformLocation(mainProgram, "uCoarseDepth"), 1);

        glUniform2f(
            glGetUniformLocation(mainProgram, "uResolution"), 
            (float)fbWidth, (float)fbHeight
        );
        glUniform3f(glGetUniformLocation(mainProgram, "uCamPos"), camPos.x, camPos.y, camPos.z);
        glUniform3f(glGetUniformLocation(mainProgram, "uCamForward"), fwd.x, fwd.y, fwd.z);
        glUniform3f(glGetUniformLocation(mainProgram, "uCamRight"), right.x, right.y, right.z);
        glUniform3f(glGetUniformLocation(mainProgram, "uCamUp"), up.x, up.y, up.z);
        glUniform1f(glGetUniformLocation(mainProgram, "uTime"), globalTime);
        glUniform3f(
            glGetUniformLocation(mainProgram, "uWaveOrigin"), 
            waveOrigin.x, waveOrigin.y, waveOrigin.z
        );
        glUniform1f(glGetUniformLocation(mainProgram, "uWaveRadius"), waveRadius);
        glUniform1i(
            glGetUniformLocation(mainProgram, "uWaveActive"), 
            waveFrontActive ? 1 : 0
        );
        glUniform1i(
            glGetUniformLocation(mainProgram, "uCityMode"), 
            cityModeEnabled ? 1 : 0
        );

        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    if (audioReady) {
        waveOutReset(hWaveOut);
        waveOutUnprepareHeader(hWaveOut, &hdr0, sizeof(WAVEHDR));
        waveOutUnprepareHeader(hWaveOut, &hdr1, sizeof(WAVEHDR));
        waveOutClose(hWaveOut);
    }
    if (udpSocket != INVALID_SOCKET) closesocket(udpSocket);
    WSACleanup();
    glfwTerminate();
    return 0;
}
