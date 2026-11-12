#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <cstdlib>

const unsigned int SCR_WIDTH = 1280;
const unsigned int SCR_HEIGHT = 720;

struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };

// 3D Spatial Hash Grid (1024 бакета по 4 слота = до 4096 дельт за O(1))
const int HASH_BUCKETS = 1024;
const int BUCKET_SLOTS = 4;
const float CELL_SIZE = 500.0f;

std::vector<Vec4> hashGridData(HASH_BUCKETS * BUCKET_SLOTS, { 0.0f, 0.0f, 0.0f, 0.0f });
int totalActiveCraters = 0;
bool hashGridDirty = true;

// Состояние расширяющегося волнового фронта (Quantum Walk Expansion)
bool waveFrontActive = false;
Vec3 waveOrigin = { 0.0f, 0.0f, 1000.0f };
float waveRadius = 0.0f;
float waveFrontSpeed = 45000.0f;

Vec3 camPos = { 0.0f, 0.0f, 1000.0f };
Vec3 camVel = { 0.0f, 0.0f, 0.0f };
bool physicsCollisionEnabled = true;

float yaw = 0.0f, pitch = 0.0f;
float lastX = SCR_WIDTH / 2.0f, lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;
float deltaTime = 0.0f, lastFrame = 0.0f;
float globalTime = 0.0f;

unsigned int ComputeSpatialHash(int ix, int iy, int iz) {
    unsigned int ux = static_cast<unsigned int>(ix + 100000);
    unsigned int uy = static_cast<unsigned int>(iy + 100000);
    unsigned int uz = static_cast<unsigned int>(iz + 100000);
    unsigned int h = (ux * 73856093u) ^ (uy * 19349663u) ^ (uz * 83492791u);
    return h % HASH_BUCKETS;
}

void AddCraterToGrid(Vec3 pos, float spawnTime) {
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
            float dx = p.x - c.x;
            float dy = p.y - c.y;
            float dz = p.z - c.z;
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

    float gx = p.x * 0.0006f;
    float gy = p.y * 0.0006f;
    float gz = p.z * 0.0006f;
    float dotVal = std::sin(gx) * std::cos(gy) + std::sin(gy) * std::cos(gz) + std::sin(gz) * std::cos(gx);
    float gyroid = std::abs(dotVal) / 0.0006f;

    float sx = p.x - std::sin(p.z * 0.0002f) * 1800.0f;
    float sy = p.y - std::cos(p.z * 0.00015f) * 1200.0f;
    float mainShaft = std::sqrt(sx * sx + sy * sy) - 950.0f;

    float sdfCaves = std::min(mainShaft, gyroid - 420.0f);
    float zBounds = std::max(-p.z, p.z - 105000.0f);
    sdfCaves = std::max(sdfCaves, zBounds);

    float baseSDF = std::max(sdfPlanet, -sdfCaves);
    return baseSDF + EvalCratersCPU(p);
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

const char* fragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
in vec2 uv;

uniform vec2 uResolution;
uniform vec3 uCamPos;
uniform vec3 uCamForward;
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform samplerBuffer uHashGrid;
uniform float uTime;
uniform vec3 uWaveOrigin;
uniform float uWaveRadius;
uniform int uWaveActive;

// Ортогональная матрица вращения SO(3) для фрактальных октав Эшера
const mat3 rotSO3 = mat3(
     0.36,  0.48, -0.80,
    -0.80,  0.60,  0.00,
     0.48,  0.64,  0.60
);

float EscherTensorNoise(vec3 p) {
    vec3 q = p * 0.032;
    float n1 = sin(q.x) * cos(q.y) * sin(q.z);
    q = rotSO3 * q * 2.13;
    float n2 = sin(q.x) * cos(q.y) * sin(q.z) * 0.45;
    q = rotSO3 * q * 2.13;
    float n3 = cos(q.x) * sin(q.y) * cos(q.z) * 0.20;
    return n1 + n2 + n3;
}

// Физический спектр излучения абсолютно черного тела (температура в Кельвинах)
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
    return h % 1024u;
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
            float dist = length(p - c.xyz);
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

float GetDistanceAndPhysics(vec3 p, out vec3 outMagField, out float outTempK) {
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
    return baseSDF + microDetails + deltaDestruction;
}

vec3 CalculateNormal(vec3 p) {
    vec2 e = vec2(0.3, 0.0);
    vec3 m; float temp;
    float d = GetDistanceAndPhysics(p, m, temp);
    return normalize(vec3(
        GetDistanceAndPhysics(p + e.xyy, m, temp) - d,
        GetDistanceAndPhysics(p + e.yxy, m, temp) - d,
        GetDistanceAndPhysics(p + e.yyx, m, temp) - d
    ));
}

float CalculateAO(vec3 p, vec3 n) {
    float occ = 0.0;
    float sca = 1.0;
    vec3 m; float temp;
    for (int i = 1; i <= 3; i++) {
        float h = 10.0 * float(i);
        float d = GetDistanceAndPhysics(p + h * n, m, temp);
        occ += (h - d) * sca;
        sca *= 0.6;
    }
    return clamp(1.0 - 0.025 * occ, 0.2, 1.0);
}

void main() {
    vec2 screenUV = uv * vec2(uResolution.x / uResolution.y, 1.0);
    vec3 rayDir = normalize(
        uCamForward + screenUV.x * uCamRight + screenUV.y * uCamUp
    );
    
    float t = 0.0;
    vec3 p = uCamPos;
    vec3 magField = vec3(0.0);
    float tempK = 0.0;
    float d = 0.0;
    
    // Квантовое туннелирование луча: 85 адаптивных шагов (ускорение до 60 FPS)
    for (int i = 0; i < 85; i++) {
        p = uCamPos + rayDir * t;
        d = GetDistanceAndPhysics(p, magField, tempK);
        float stepScale = (d > 150.0) ? 1.15 : 0.78;
        t += d * stepScale;
        if (d < 0.25 || t > 120000.0) break;
    }
    
    vec3 fogColor = vec3(0.03, 0.06, 0.12);
    vec3 col = fogColor;
    
    if (d < 0.25) {
        vec3 normal = CalculateNormal(p);
        float ao = CalculateAO(p, normal);
        vec3 lightDir = normalize((uCamPos + uCamForward * 800.0) - p);
        float diff = max(0.18, dot(normal, lightDir));
        float fog = exp(-t * 0.000045);
        float magLine = pow(abs(sin(magField.x * 3.0 + magField.y * 3.0)), 10.0);
        
        vec3 rockCol = vec3(0.44, 0.42, 0.47) * diff * ao;
        vec3 magCol = vec3(0.0, 0.72, 1.0) * magLine;
        vec3 thermalGlow = vec3(0.0);
        
        if (tempK > 700.0) {
            float intensity = clamp((tempK - 700.0) / 1400.0, 0.0, 1.8);
            thermalGlow = BlackbodyRadiation(tempK) * intensity;
        }
        
        col = mix(fogColor, rockCol + magCol + thermalGlow, fog);
    }
    
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
        AddCraterToGrid(target, globalTime);
    }
}

void TriggerOrbitalBombardment(float currentTime) {
    for (int i = 0; i < 250; i++) {
        float zPos = 1500.0f + static_cast<float>(i) * 410.0f;
        float cx = std::sin(zPos * 0.0002f) * 1800.0f;
        float cy = std::cos(zPos * 0.00015f) * 1200.0f - 850.0f;
        float offsetX = static_cast<float>((std::rand() % 1200) - 600);
        AddCraterToGrid({ cx + offsetX, cy, zPos }, currentTime);
    }
}

int main() {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(
        SCR_WIDTH, SCR_HEIGHT, "Anatomical Engine v3.0", NULL, NULL
    );
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexShaderSource, NULL); 
    glCompileShader(vs);

    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentShaderSource, NULL); 
    glCompileShader(fs);

    unsigned int shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs); 
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);

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
        -1.0f,  1.0f, 
        -1.0f, -1.0f, 
         1.0f, -1.0f, 
        -1.0f,  1.0f, 
         1.0f, -1.0f, 
         1.0f,  1.0f 
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
    bool cKeyPressed = false;
    bool bKeyPressed = false;
    bool xKeyPressed = false;

    while (!glfwWindowShouldClose(window)) {
        globalTime = static_cast<float>(glfwGetTime());
        deltaTime = std::min(globalTime - lastFrame, 0.05f);
        lastFrame = globalTime;

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, true);
        }
        if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS && !cKeyPressed) {
            physicsCollisionEnabled = !physicsCollisionEnabled;
            cKeyPressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_C) == GLFW_RELEASE) {
            cKeyPressed = false;
        }
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS && !bKeyPressed) {
            TriggerOrbitalBombardment(globalTime);
            bKeyPressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_RELEASE) {
            bKeyPressed = false;
        }
        // Запуск баллистического теплового фронта на клавише X
        if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS && !xKeyPressed) {
            waveFrontActive = true;
            waveOrigin = camPos;
            waveRadius = 50.0f;
            xKeyPressed = true;
        }

        if (glfwGetKey(window, GLFW_KEY_X) == GLFW_RELEASE) {
            xKeyPressed = false;
        }
        if (waveFrontActive) {
            waveRadius += waveFrontSpeed * deltaTime;
            if (waveRadius > 108000.0f) {
                waveFrontActive = false;
    }
}


        if (hashGridDirty) {
            glBindBuffer(GL_TEXTURE_BUFFER, hashTBO);
            glBufferSubData(
                GL_TEXTURE_BUFFER, 
                0, 
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

        // Тяга корабля и инерционная физика
        bool isBoost = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
        float thrust = isBoost ? 25000.0f : 3500.0f;
        Vec3 accel = { 0.0f, 0.0f, 0.0f };

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { 
            accel.x += fwd.x * thrust; 
            accel.y += fwd.y * thrust; 
            accel.z += fwd.z * thrust; 
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { 
            accel.x -= fwd.x * thrust; 
            accel.y -= fwd.y * thrust; 
            accel.z -= fwd.z * thrust; 
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { 
            accel.x -= right.x * thrust; 
            accel.y -= right.y * thrust; 
            accel.z -= right.z * thrust; 
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { 
            accel.x += right.x * thrust; 
            accel.y += right.y * thrust; 
            accel.z += right.z * thrust; 
        }

        if (physicsCollisionEnabled) {
            accel.y -= 180.0f;
        }

        camVel.x = (camVel.x + accel.x * deltaTime) * 0.94f;
        camVel.y = (camVel.y + accel.y * deltaTime) * 0.94f;
        camVel.z = (camVel.z + accel.z * deltaTime) * 0.94f;

        camPos.x += camVel.x * deltaTime;
        camPos.y += camVel.y * deltaTime;
        camPos.z += camVel.z * deltaTime;

        // Аналитическая физика столкновений по градиенту SDF
        float hullRadius = 25.0f;
        float sdfDist = EvaluateWorldSDF_CPU(camPos);
        if (physicsCollisionEnabled && sdfDist < hullRadius) {
            Vec3 normal = ComputeGradientCPU(camPos);
            float penetration = hullRadius - sdfDist;
            camPos.x += normal.x * penetration;
            camPos.y += normal.y * penetration;
            camPos.z += normal.z * penetration;

            float vDotN = camVel.x * normal.x + 
                          camVel.y * normal.y + 
                          camVel.z * normal.z;
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
            "Anatomical v3.0 | Depth: %.1f km | Craters: %d [B] | Wave: %s [X] | FPS: %d",
            camPos.z / 1000.0f, 
            totalActiveCraters, 
            waveFrontActive ? "ACTIVE" : "READY", 
            fps
        );
        glfwSetWindowTitle(window, titleBuffer.data());

        int fbWidth, fbHeight;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);

        glUseProgram(shaderProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_BUFFER, hashTex);
        glUniform1i(glGetUniformLocation(shaderProgram, "uHashGrid"), 0);

        glUniform2f(
            glGetUniformLocation(shaderProgram, "uResolution"), 
            static_cast<float>(fbWidth), 
            static_cast<float>(fbHeight)
        );
        glUniform3f(
            glGetUniformLocation(shaderProgram, "uCamPos"), 
            camPos.x, camPos.y, camPos.z
        );
        glUniform3f(
            glGetUniformLocation(shaderProgram, "uCamForward"), 
            fwd.x, fwd.y, fwd.z
        );
        glUniform3f(
            glGetUniformLocation(shaderProgram, "uCamRight"), 
            right.x, right.y, right.z
        );
        glUniform3f(
            glGetUniformLocation(shaderProgram, "uCamUp"), 
            up.x, up.y, up.z
        );
        glUniform1f(glGetUniformLocation(shaderProgram, "uTime"), globalTime);
        glUniform3f(
            glGetUniformLocation(shaderProgram, "uWaveOrigin"),
            waveOrigin.x, waveOrigin.y, waveOrigin.z
        );
        glUniform1f(
            glGetUniformLocation(shaderProgram, "uWaveRadius"),
            waveRadius
        );

        glUniform1i(
            glGetUniformLocation(shaderProgram, "uWaveActive"), 
            waveFrontActive ? 1 : 0
        );

        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}
