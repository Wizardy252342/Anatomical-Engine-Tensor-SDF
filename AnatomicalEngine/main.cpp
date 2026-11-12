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

const unsigned int SCR_WIDTH = 1280;
const unsigned int SCR_HEIGHT = 720;
const unsigned int COARSE_WIDTH = 320;
const unsigned int COARSE_HEIGHT = 180;

struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };

// Выровненный под 4 кэш-линии 256-байтный сетевой пакет высокой точности (ESNPacket256)
struct alignas(64) ESNPacket256 {
    uint32_t magic;            // 4 байта: 0x45534E35 ('ESN5')
    uint32_t senderId;         // 4 байта: ID отправителя
    uint32_t opType;           // 4 байта: 1=кратер, 2=залп(B), 3=волна(X)
    uint32_t seed;             // 4 байта: детерминированное зерно
    float posX;                // 4 байта
    float posY;                // 4 байта
    float posZ;                // 4 байта
    float temperatureK;        // 4 байта: пиковая температура в Кельвинах
    // 16 коэффициентов тензорной деформации и асимметрии кратера (64 байта)
    float t00, t01, t02, t03;
    float t10, t11, t12, t13;
    float t20, t21, t22, t23;
    float t30, t31, t32, t33;
    // Расширенный резерв тензорных гармоник (160 байт -> итого ровно 256 байт!)
    uint64_t harmonicReservoir0;
    uint64_t harmonicReservoir1;
    uint64_t harmonicReservoir2;
    uint64_t harmonicReservoir3;
    uint64_t harmonicReservoir4;
    uint64_t harmonicReservoir5;
    uint64_t harmonicReservoir6;
    uint64_t harmonicReservoir7;
    uint64_t harmonicReservoir8;
    uint64_t harmonicReservoir9;
    uint64_t harmonicReservoir10;
    uint64_t harmonicReservoir11;
    uint64_t harmonicReservoir12;
    uint64_t harmonicReservoir13;
    uint64_t harmonicReservoir14;
    uint64_t harmonicReservoir15;
    uint64_t harmonicReservoir16;
    uint64_t harmonicReservoir17;
    uint64_t harmonicReservoir18;
    uint64_t harmonicReservoir19;
};
static_assert(sizeof(ESNPacket256) == 256, "ESNPacket256 must be exactly 256 bytes");

// --- Встроенный процедурный синтезатор звука (Pure Math Audio Engine) ---
std::atomic<float> audioExplosionEnv{ 0.0f };
std::atomic<float> audioWaveEnv{ 0.0f };
std::atomic<float> audioEngineThrust{ 0.0f };
std::atomic<float> audioCavernDepth{ 0.0f };
std::atomic<bool> audioRunning{ true };

void PlayProceduralSoundBuffer(HWAVEOUT hWaveOut, WAVEHDR* hdr, std::vector<int16_t>& buf, double& phase1, double& phase2) {
    const double sampleRate = 22050.0;
    float expEnv = audioExplosionEnv.load();
    float wavEnv = audioWaveEnv.load();
    float thrust = audioEngineThrust.load();
    float depth = audioCavernDepth.load();

    for (size_t i = 0; i < buf.size(); i++) {
        // 1. Низкочастотный гул двигателей и резонанс пещеры (45 Гц - 130 Гц)
        double engineFreq = 48.0 + thrust * 65.0 + std::min(depth * 0.25, 35.0);
        phase1 += (2.0 * 3.14159265 * engineFreq) / sampleRate;
        double engineHum = std::sin(phase1) * (0.14 + thrust * 0.18);

        // 2. Акустический удар 256-байтного кратера (суб-бас + шум разлома породы)
        double explosionSound = 0.0;
        if (expEnv > 0.001f) {
            double boomFreq = 32.0 + expEnv * 95.0;
            phase2 += (2.0 * 3.14159265 * boomFreq) / sampleRate;
            double whiteNoise = (static_cast<double>(std::rand() % 2000) / 1000.0) - 1.0;
            explosionSound = (std::sin(phase2) * 0.7 + whiteNoise * 0.35) * expEnv * 0.65;
            expEnv *= 0.99965f;
        }

        // 3. Интерференционный гул сферической тепловой волны (клавиша X)
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

// Сетевые переменные UDP
SOCKET udpSocket = INVALID_SOCKET;
uint32_t localNodeId = 0;
uint16_t boundPort = 0;
int networkPacketsSent = 0;
int networkPacketsRecv = 0;

// Расширенная 3D Spatial Hash Grid (16384 бакета x 4 слота = 65536 дельт за O(1))
const int HASH_BUCKETS = 16384;
const int BUCKET_SLOTS = 4;
const float CELL_SIZE = 500.0f;

std::vector<Vec4> hashGridData(HASH_BUCKETS * BUCKET_SLOTS, { 0.0f, 0.0f, 0.0f, 0.0f });
int totalActiveCraters = 0;
bool hashGridDirty = true;

bool waveFrontActive = false;
Vec3 waveOrigin = { 0.0f, 0.0f, 1000.0f };
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
    pkt.magic = 0x45534E35;
    pkt.senderId = localNodeId;
    pkt.opType = opType;
    pkt.seed = seedVal;
    pkt.posX = pos.x;
    pkt.posY = pos.y;
    pkt.posZ = pos.z;
    pkt.temperatureK = tempK;
    // Заполняем тензор асимметрии кратера высокой точности
    pkt.t00 = 1.15f; pkt.t11 = 0.92f; pkt.t22 = 1.08f; pkt.t33 = 1.0f;

    uint16_t portA = 7777;
    uint16_t portB = 7778;
    uint16_t targetPort = (boundPort == portA) ? portB : portA;

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

unsigned int ComputeSpatialHash(int ix, int iy, int iz) {
    unsigned int ux = static_cast<unsigned int>(ix + 100000);
    unsigned int uy = static_cast<unsigned int>(iy + 100000);
    unsigned int uz = static_cast<unsigned int>(iz + 100000);
    unsigned int h = (ux * 73856093u) ^ (uy * 19349663u) ^ (uz * 83492791u);
    return h % HASH_BUCKETS;
}

void AddCraterToGrid(Vec3 pos, float spawnTime, bool playSound = true) {
    int cx = static_cast<int>(std::floor(pos.x / CELL_SIZE));
    int cy = static_cast<int>(std::floor(pos.y / CELL_SIZE));
    int cz = static_cast<int>(std::floor(pos.z / CELL_SIZE));
    unsigned int bucket = ComputeSpatialHash(cx, cy, cz);

    int baseIdx = static_cast<int>(bucket) * BUCKET_SLOTS;
    for (int i = BUCKET_SLOTS - 1; i > 0; i--) {
        hashGridData.at(baseIdx + i) = hashGridData.at(baseIdx + i - 1);
    }
    hashGridData.at(baseIdx) = { pos.x, pos.y, pos.z, spawnTime + 1.0f };
    totalActiveCraters++;
    hashGridDirty = true;

    if (playSound) {
        float dx = pos.x - camPos.x;
        float dy = pos.y - camPos.y;
        float dz = pos.z - camPos.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float atten = 1.0f / (1.0f + dist * 0.0004f);
        audioExplosionEnv.store(std::min(1.0f, audioExplosionEnv.load() + atten));
    }
}

void TriggerOrbitalBombardment(float currentTime, uint32_t seedVal) {
    std::srand(seedVal);
    for (int i = 0; i < 250; i++) {
        float zPos = 1500.0f + static_cast<float>(i) * 410.0f;
        float cx = std::sin(zPos * 0.0002f) * 1800.0f;
        float cy = std::cos(zPos * 0.00015f) * 1200.0f - 850.0f;
        float offsetX = static_cast<float>((std::rand() % 1200) - 600);
        AddCraterToGrid({ cx + offsetX, cy, zPos }, currentTime, false);
    }
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
        if (pkt.magic != 0x45534E35 || pkt.senderId == localNodeId) continue;

        networkPacketsRecv++;
        Vec3 remotePos = { pkt.posX, pkt.posY, pkt.posZ };
        if (pkt.opType == 1) {
            AddCraterToGrid(remotePos, globalTime, true);
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
float EvalCratersCPU(Vec3 p) {
    int cx = static_cast<int>(std::floor(p.x / CELL_SIZE));
    int cy = static_cast<int>(std::floor(p.y / CELL_SIZE));
    int cz = static_cast<int>(std::floor(p.z / CELL_SIZE));
    unsigned int bucket = ComputeSpatialHash(cx, cy, cz);
    int baseIdx = static_cast<int>(bucket) * BUCKET_SLOTS;

    float totalDelta = 0.0f;
    for (int i = 0; i < BUCKET_SLOTS; i++) {
        Vec4 c = hashGridData.at(baseIdx + i);
        if (c.w > 0.5f) {
            float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq < 480.0f * 480.0f) {
                totalDelta += 230.0f * std::exp(-distSq / (2.0f * 115.0f * 115.0f));
            }
        }
    }
    return totalDelta;
}

float EvaluateWorldSDF_CPU(Vec3 p) {
    float py = p.y + 15000000.0f;
    float sdfPlanet = std::sqrt(p.x * p.x + py * py + p.z * p.z) - 15000000.0f;
    float gx = p.x * 0.0006f, gy = p.y * 0.0006f, gz = p.z * 0.0006f;
    float dotVal = std::sin(gx) * std::cos(gy) + std::sin(gy) * std::cos(gz) + std::sin(gz) * std::cos(gx);
    float gyroid = std::abs(dotVal) / 0.0006f;

    float sx = p.x - std::sin(p.z * 0.0002f) * 1800.0f;
    float sy = p.y - std::cos(p.z * 0.00015f) * 1200.0f;
    float mainShaft = std::sqrt(sx * sx + sy * sy) - 950.0f;

    float sdfCaves = std::min(mainShaft, gyroid - 420.0f);
    float zBounds = std::max(-p.z, p.z - 105000.0f);
    sdfCaves = std::max(sdfCaves, zBounds);

    return std::max(sdfPlanet, -sdfCaves) + EvalCratersCPU(p);
}

Vec3 ComputeGradientCPU(Vec3 p) {
    float e = 0.6f;
    float d = EvaluateWorldSDF_CPU(p);
    float dx = EvaluateWorldSDF_CPU({ p.x + e, p.y, p.z }) - d;
    float dy = EvaluateWorldSDF_CPU({ p.x, p.y + e, p.z }) - d;
    float dz = EvaluateWorldSDF_CPU({ p.x, p.y, p.z + e }) - d;
    float l = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (l > 0.00001f) return { dx / l, dy / l, dz / l };
    return { 0.0f, 1.0f, 0.0f };
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

// Процедурный SDF мегаполиса ArcCorp (небоскрёбы, шпили, шахты в пещеры)
float ArcCorpCitySDF(vec3 p, out vec3 outCityGlow) {
    outCityGlow = vec3(0.0);
    // Сетка городских кварталов 220 x 220 метров
    float blockSize = 220.0;
    vec2 cellID = floor(p.xz / blockSize);
    vec2 localXZ = mod(p.xz, blockSize) - blockSize * 0.5;
    
    float rnd = Hash21(cellID);
    float rnd2 = Hash21(cellID + vec2(17.3, 41.9));
    
    // Оставляем каждую 12-ю ячейку как гигантскую шахту-вход в 105-км подземные пещеры!
    if (rnd > 0.91 && p.z > 0.0 && p.z < 105000.0) {
        return 150.0; 
    }
    
    // Высота небоскрёба (от 120 до 780 метров)
    float bHeight = 120.0 + pow(rnd, 1.8) * 680.0;
    vec2 bHalfSize = vec2(58.0 + rnd2 * 28.0, 58.0 + rnd * 28.0);
    
    // Основной корпус башни (Box SDF)
    vec3 q = abs(vec3(localXZ.x, p.y - bHeight * 0.5, localXZ.y)) - vec3(bHalfSize.x, bHeight * 0.5, bHalfSize.y);
    float towerSDF = length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
    
    // Надстройка / шпиль на крыше высоких башен
    if (rnd > 0.55) {
        vec3 qTop = abs(vec3(localXZ.x, p.y - (bHeight + 65.0), localXZ.y)) - vec3(bHalfSize.x * 0.45, 65.0, bHalfSize.y * 0.45);
        float spireSDF = length(max(qTop, 0.0)) + min(max(qTop.x, max(qTop.y, qTop.z)), 0.0);
        towerSDF = min(towerSDF, spireSDF);
    }
    
    // Нижняя плита города (уровень земли y = 0)
    float groundPlate = p.y;
    float citySDF = min(towerSDF, groundPlate);
    
    // Расчёт светящихся неоновых окон и воздушных трасс транспорта
    if (towerSDF < 3.0 && p.y > 15.0 && p.y < bHeight - 8.0) {
        float winY = fract(p.y * 0.14);
        float winX = fract((localXZ.x + localXZ.y) * 0.12);
        float winMask = step(0.35, winY) * step(0.3, winX);
        float lit = step(0.45, Hash21(floor(vec2((localXZ.x + localXZ.y) * 0.12, p.y * 0.14)) + cellID));
        vec3 winColor = (rnd2 > 0.5) ? vec3(1.0, 0.65, 0.25) : vec3(0.25, 0.85, 1.0);
        outCityGlow += winColor * winMask * lit * 1.4;
    }
    
    // Авиационные маяки на вершинах башен
    if (p.y > bHeight - 4.0 && length(localXZ) < 12.0) {
        float pulse = 0.5 + 0.5 * sin(uTime * 5.0 + rnd * 10.0);
        outCityGlow += vec3(1.0, 0.12, 0.08) * pulse * 2.5;
    }
    
    // Световые потоки летающих спидеров в каньонах улиц (на высотах 140м и 280м)
    float laneH = abs(p.y - (140.0 + floor(rnd2 * 3.0) * 90.0));
    if (laneH < 6.0 && (abs(localXZ.x) > bHalfSize.x + 8.0 || abs(localXZ.y) > bHalfSize.y + 8.0)) {
        float traffic1 = pow(max(0.0, sin(p.z * 0.03 - uTime * 18.0 + cellID.x)), 16.0);
        float traffic2 = pow(max(0.0, sin(p.x * 0.03 + uTime * 22.0 + cellID.y)), 16.0);
        outCityGlow += vec3(1.0, 0.2, 0.1) * traffic1 * 2.0 + vec3(0.2, 0.8, 1.0) * traffic2 * 2.0;
    }
    
    return citySDF;
}

float LiquidMetalOceanSDF(vec3 p) {
    float wave1 = sin(p.x * 0.025 + uTime * 1.8) * cos(p.z * 0.025 - uTime * 1.3) * 4.5;
    float wave2 = sin((p.x + p.z) * 0.07 - uTime * 3.1) * 1.4;
    return (p.y + 620.0) - (wave1 + wave2);
}

// Материалы: 1 = скала, 2 = жидкий металл, 3 = архитектура города ArcCorp
float GetDistanceAndPhysics(vec3 p, out vec3 outMagField, out float outTempK, out int outMatID, out vec3 outCityGlow) {
    outCityGlow = vec3(0.0);
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
    
    // Если включён режим города (uCityMode == 1) и мы выше уровня земли (-20м)
    if (uCityMode == 1 && p.y > -20.0) {
        float citySDF = ArcCorpCitySDF(p, outCityGlow) + deltaDestruction;
        outMatID = 3;
        return citySDF;
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
    vec3 m, cg; float temp; int mat;
    float d = GetDistanceAndPhysics(p, m, temp, mat, cg);
    return normalize(vec3(
        GetDistanceAndPhysics(p + e.xyy, m, temp, mat, cg) - d,
        GetDistanceAndPhysics(p + e.yxy, m, temp, mat, cg) - d,
        GetDistanceAndPhysics(p + e.yyx, m, temp, mat, cg) - d
    ));
}

float CalculateAO(vec3 p, vec3 n) {
    float occ = 0.0, sca = 1.0;
    vec3 m, cg; float temp; int mat;
    for (int i = 1; i <= 3; i++) {
        float h = 10.0 * float(i);
        float d = GetDistanceAndPhysics(p + h * n, m, temp, mat, cg);
        occ += (h - d) * sca;
        sca *= 0.6;
    }
    return clamp(1.0 - 0.025 * occ, 0.2, 1.0);
}

vec3 TraceSecondaryReflection(vec3 origin, vec3 reflDir) {
    float t = 5.0;
    vec3 p = origin, magField = vec3(0.0), cg = vec3(0.0);
    float tempK = 0.0, d = 1000.0;
    int matID = 1;
    
    for (int i = 0; i < 24; i++) {
        p = origin + reflDir * t;
        d = GetDistanceAndPhysics(p, magField, tempK, matID, cg);
        t += d * 1.15;
        if (d < 1.5 || t > 35000.0) break;
    }
    
    vec3 bg = vec3(0.02, 0.05, 0.11);
    if (d < 1.5) {
        float fog = exp(-t * 0.00006);
        float magLine = pow(abs(sin(magField.x * 3.0 + magField.y * 3.0)), 10.0);
        vec3 rock = vec3(0.28, 0.30, 0.36) + cg;
        vec3 mag = vec3(0.0, 0.72, 1.0) * magLine;
        vec3 glow = (tempK > 700.0) ? BlackbodyRadiation(tempK) * 1.2 : vec3(0.0);
        return mix(bg, rock + mag + glow, fog);
    }
    return bg;
}

void main() {
    vec2 screenUV = uv * vec2(uResolution.x / uResolution.y, 1.0);
    vec3 rayDir = normalize(
        uCamForward + screenUV.x * uCamRight + screenUV.y * uCamUp
    );
    
    vec2 texUV = uv * 0.5 + 0.5;
    float t = (uCityMode == 1) ? 0.0 : texture(uCoarseDepth, texUV).r;
    
    if (uCityMode == 0 && rayDir.y < -0.02 && uCamPos.y > -620.0) {
        float tOcean = (-620.0 - uCamPos.y) / rayDir.y;
        t = min(t, max(0.0, tOcean - 50.0));
    }
    
    vec3 p = uCamPos, magField = vec3(0.0), cityGlow = vec3(0.0);
    float tempK = 0.0, d = 1000.0;
    int matID = 1;
    
    vec3 volumetricAccum = vec3(0.0);
    float transmittance = 1.0;
    
    if (t < 119500.0) {
        for (int i = 0; i < 65; i++) {
            p = uCamPos + rayDir * t;
                        d = GetDistanceAndPhysics(p, magField, tempK, matID, cityGlow);
            
            if (tempK > 1200.0) {
                vec3 heatGrad = vec3(
                    sin(p.y * 0.05 + uTime * 8.0), 
                    cos(p.x * 0.05), 
                    0.0
                );
                float heatScale = clamp((tempK - 1200.0) / 2000.0, 0.0, 1.0);
                rayDir = normalize(rayDir + heatGrad * 0.00035 * heatScale);
            }
            
            float stepLen = max(1.5, d * 0.85);
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
    
    float distCenter = length(screenUV);
    if (abs(distCenter - 0.025) < 0.0015 || (distCenter < 0.003)) {
        col = mix(col, vec3(0.2, 0.9, 1.0), 0.7);
    }
    FragColor = vec4(col, 1.0);
}
)";

Vec3 Cross(Vec3 a, Vec3 b) {
    return { 
        a.y * b.z - a.z * b.y, 
        a.z * b.x - a.x * b.z, 
        a.x * b.y - a.y * b.x 
    };
}

Vec3 Normalize(Vec3 v) {
    float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l > 0.0f) return { v.x / l, v.y / l, v.z / l };
    return { 0.0f, 0.0f, 0.0f };
}

void mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);
        if (firstMouse) { 
        lastX = xpos; 
        lastY = ypos; 
        firstMouse = false; 
    }
    yaw += (xpos - lastX) * 0.15f;
    pitch += (lastY - ypos) * 0.15f;
    lastX = xpos; 
    lastY = ypos;
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
        Vec3 target = {
            camPos.x + fwd.x * 1200.0f,
            camPos.y + fwd.y * 1200.0f,
            camPos.z + fwd.z * 1200.0f
        };
        AddCraterToGrid(target, globalTime, true);
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
    WAVEHDR hdr0{};
    WAVEHDR hdr1{};
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
        SCR_WIDTH, SCR_HEIGHT, "Anatomical Engine v5.0 (256B + Audio)", NULL, NULL
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
        hashGridData.size() * sizeof(Vec4), 
        hashGridData.data(), 
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
    bool cKeyPressed = false, bKeyPressed = false, xKeyPressed = false;

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
        static bool mKeyPressed = false;
        if (glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS && !mKeyPressed) {
            cityModeEnabled = !cityModeEnabled;
            mKeyPressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_M) == GLFW_RELEASE) {
            mKeyPressed = false;
        }


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

        if (hashGridDirty) {
            glBindBuffer(GL_TEXTURE_BUFFER, hashTBO);
            glBufferSubData(
                GL_TEXTURE_BUFFER, 0, 
                hashGridData.size() * sizeof(Vec4), 
                hashGridData.data()
            );
            hashGridDirty = false;
        }

        float radYaw = yaw * 3.14159f / 180.0f;
        float radPitch = pitch * 3.14159f / 180.0f;
        Vec3 rawFwd = {
            std::sin(radYaw) * std::cos(radPitch),
            std::sin(radPitch),
            std::cos(radYaw) * std::cos(radPitch)
        };
        Vec3 fwd = Normalize(rawFwd);
        Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
        Vec3 right = Normalize(Cross(fwd, worldUp));
        Vec3 up = Normalize(Cross(right, fwd));

        bool isBoost = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
        float thrust = isBoost ? 25000.0f : 3500.0f;
        Vec3 accel = { 0.0f, 0.0f, 0.0f };

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { 
            accel.x += fwd.x * thrust; accel.y += fwd.y * thrust; accel.z += fwd.z * thrust; 
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { 
            accel.x -= fwd.x * thrust; accel.y -= fwd.y * thrust; accel.z -= fwd.z * thrust; 
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { 
            accel.x -= right.x * thrust; accel.y -= right.y * thrust; accel.z -= right.z * thrust; 
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { 
            accel.x += right.x * thrust; accel.y += right.y * thrust; accel.z += right.z * thrust; 
        }

        if (physicsCollisionEnabled) accel.y -= 180.0f;

        camVel.x = (camVel.x + accel.x * deltaTime) * 0.94f;
        camVel.y = (camVel.y + accel.y * deltaTime) * 0.94f;
        camVel.z = (camVel.z + accel.z * deltaTime) * 0.94f;

        camPos.x += camVel.x * deltaTime;
        camPos.y += camVel.y * deltaTime;
        camPos.z += camVel.z * deltaTime;

        audioEngineThrust.store(isBoost ? 1.0f : 0.15f);
        audioCavernDepth.store(std::max(0.0f, camPos.z / 1000.0f));

        float hullRadius = 25.0f;
        float sdfDist = EvaluateWorldSDF_CPU(camPos);
        if (physicsCollisionEnabled && sdfDist < hullRadius) {
            Vec3 normal = ComputeGradientCPU(camPos);
            float penetration = hullRadius - sdfDist;
            camPos.x += normal.x * penetration;
            camPos.y += normal.y * penetration;
            camPos.z += normal.z * penetration;

            float vDotN = camVel.x * normal.x + camVel.y * normal.y + camVel.z * normal.z;
            if (vDotN < 0.0f) {
                camVel.x -= 1.3f * vDotN * normal.x;
                camVel.y -= 1.3f * vDotN * normal.y;
                camVel.z -= 1.3f * vDotN * normal.z;
            }
        }

        int fps = (deltaTime > 0.0f) ? static_cast<int>(1.0f / deltaTime) : 0;
        snprintf(
            titleBuffer.data(), 
            titleBuffer.size(),
            "Anatomical v5.0 (256B+Audio) | Port:%d | Tx/Rx:%d/%d | Craters:%d | FPS:%d",
            boundPort, networkPacketsSent, networkPacketsRecv, totalActiveCraters, fps
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
