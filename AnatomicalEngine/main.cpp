#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include <vector>
#include <cstdio>

const unsigned int SCR_WIDTH = 1280;
const unsigned int SCR_HEIGHT = 720;

struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };

// Стартовая позиция внутри гигантского 105-км разлома
Vec3 camPos = { 0.0f, 0.0f, 1000.0f };
float yaw = 0.0f, pitch = 0.0f;
float lastX = SCR_WIDTH / 2.0f, lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;
float deltaTime = 0.0f, lastFrame = 0.0f;

// 4 независимых 64-байтных кратера разрушения (без массивов)
Vec4 crater0 = { 0.0f, 0.0f, 0.0f, 0.0f };
Vec4 crater1 = { 0.0f, 0.0f, 0.0f, 0.0f };
Vec4 crater2 = { 0.0f, 0.0f, 0.0f, 0.0f };
Vec4 crater3 = { 0.0f, 0.0f, 0.0f, 0.0f };
int nextCraterIdx = 0;

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
uniform vec4 uCrater0;
uniform vec4 uCrater1;
uniform vec4 uCrater2;
uniform vec4 uCrater3;
uniform float uTime;

// 1. Детерминированная матрица Эшера (многоуровневая микро-детализация)
float EscherNoise(vec3 p) {
    float s = 0.035;
    float n1 = sin(p.x * s) * cos(p.y * s) * sin(p.z * s);
    float n2 = cos(p.x * s * 3.1) * sin(p.y * s * 2.7) * cos(p.z * s * 3.1) * 0.4;
    return n1 + n2;
}

// Вспомогательная функция расчета кратера от 64-байтного пакета
float EvalCrater(vec3 p, vec4 c, out float glow) {
    glow = 0.0;
    if (c.w > 0.5) {
        float dist = length(p - c.xyz);
        if (dist < 450.0) {
            glow = exp(-(dist * dist) / (2.0 * 140.0 * 140.0));
            return 240.0 * exp(-(dist * dist) / (2.0 * 110.0 * 110.0));
        }
    }
    return 0.0;
}

// 2. Глобальный SDF 30,000-км планеты + 105-км 3D-система мега-пещер (Гироидный граф)
float GetDistanceAndPhysics(vec3 p, out vec3 outMagField, out float outCraterGlow) {
    float sdfPlanet = length(p + vec3(0.0, 15000000.0, 0.0)) - 15000000.0;
    
    // Аналитическая 3D-решетка гигантских подземных пустот (масштаб 105 км)
    vec3 g = p * 0.0006; 
    float gyroid = abs(dot(sin(g), cos(g.yzx))) / 0.0006;
    
    // Главный сквозной 105-километровый разлом + ответвления гироида
    float mainShaft = length(p.xy - vec2(sin(p.z * 0.0002) * 1800.0, cos(p.z * 0.00015) * 1200.0)) - 950.0;
    float sdfCaves = min(mainShaft, gyroid - 420.0);
    
    // Ограничиваем пещерную сеть длиной в 105 километров (от Z = 0 до Z = 105,000 м)
    float zBounds = max(-p.z, p.z - 105000.0);
    sdfCaves = max(sdfCaves, zBounds);
    
    // Топологическая инверсия знака
    float baseSDF = max(sdfPlanet, -sdfCaves);
    
    // Функциональное затухание шума Эшера
    float damping = exp(-0.00008 * baseSDF * baseSDF);
    float microDetails = EscherNoise(p) * 35.0 * damping;
    float combinedSDF = baseSDF + microDetails;
    
    // Применение 4 активных 64-байтных операторов разрушения
    float g0, g1, g2, g3;
    float deltaDestruction = 0.0;
    deltaDestruction += EvalCrater(p, uCrater0, g0);
    deltaDestruction += EvalCrater(p, uCrater1, g1);
    deltaDestruction += EvalCrater(p, uCrater2, g2);
    deltaDestruction += EvalCrater(p, uCrater3, g3);
    outCraterGlow = g0 + g1 + g2 + g3;
    
    // Аналитическое магнитное поле Максвелла
    outMagField = vec3(
        sin(p.y * 0.004 + p.z * 0.001 + uTime * 0.4),
        cos(p.x * 0.004 - p.z * 0.001),
        sin(p.z * 0.002)
    );
    
    return combinedSDF + deltaDestruction;
}

vec3 CalculateNormal(vec3 p) {
    vec2 e = vec2(0.2, 0.0);
    vec3 m; float c;
    float d = GetDistanceAndPhysics(p, m, c);
    return normalize(vec3(
        GetDistanceAndPhysics(p + e.xyy, m, c) - d,
        GetDistanceAndPhysics(p + e.yxy, m, c) - d,
        GetDistanceAndPhysics(p + e.yyx, m, c) - d
    ));
}

void main() {
    vec2 screenUV = uv * vec2(uResolution.x / uResolution.y, 1.0);
    vec3 rayDir = normalize(uCamForward + screenUV.x * uCamRight + screenUV.y * uCamUp);
    
    float t = 0.0;
    vec3 p, magField;
    float craterGlow = 0.0, d = 0.0;
    
    // Горизонт трассировки: до 120 километров (120,000 м)
    for (int i = 0; i < 140; i++) {
        p = uCamPos + rayDir * t;
        d = GetDistanceAndPhysics(p, magField, craterGlow);
        t += d * 0.85;
        if (d < 0.15 || t > 120000.0) break;
    }
    
    vec3 fogColor = vec3(0.03, 0.06, 0.12);
    vec3 col = fogColor;
    
    if (d < 0.15) {
        vec3 normal = CalculateNormal(p);
        vec3 lightDir = normalize((uCamPos + uCamForward * 800.0) - p);
        float diff = max(0.15, dot(normal, lightDir));
        float fog = exp(-t * 0.000045);
        float magLine = pow(abs(sin(magField.x * 3.0 + magField.y * 3.0)), 10.0);
        
        vec3 rockCol = vec3(0.42, 0.40, 0.45) * diff;
        vec3 magCol = vec3(0.0, 0.75, 1.0) * magLine;
        vec3 craterCol = vec3(1.0, 0.35, 0.05) * clamp(craterGlow, 0.0, 1.5);
        
        col = mix(fogColor, rockCol + magCol + craterCol, fog);
    }
    
    // Инженерный прицел по центру экрана (HUD)
    float distCenter = length(screenUV);
    if (abs(distCenter - 0.025) < 0.0015 || (distCenter < 0.003)) {
        col = mix(col, vec3(0.2, 0.9, 1.0), 0.7);
    }
    
    FragColor = vec4(col, 1.0);
}
)";

Vec3 Cross(Vec3 a, Vec3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

Vec3 Normalize(Vec3 v) {
    float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l > 0.0f) return { v.x / l, v.y / l, v.z / l };
    return { 0.0f, 0.0f, 0.0f };
}

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

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        float radYaw = yaw * 3.14159f / 180.0f;
        float radPitch = pitch * 3.14159f / 180.0f;
        Vec3 fwd = {
            std::sin(radYaw) * std::cos(radPitch),
            std::sin(radPitch),
            std::cos(radYaw) * std::cos(radPitch)
        };
        
        Vec4 newCrater = {
            camPos.x + fwd.x * 1500.0f,
            camPos.y + fwd.y * 1500.0f,
            camPos.z + fwd.z * 1500.0f,
            1.0f
        };
        
        if (nextCraterIdx == 0) crater0 = newCrater;
        else if (nextCraterIdx == 1) crater1 = newCrater;
        else if (nextCraterIdx == 2) crater2 = newCrater;
        else if (nextCraterIdx == 3) crater3 = newCrater;
        
        std::cout << "[ESN Packet #" << nextCraterIdx + 1 << "] 64-byte Operator dispatched at Depth: " 
                  << (newCrater.z / 1000.0f) << " km\n";
        nextCraterIdx = (nextCraterIdx + 1) % 4;
    }
}

int main() {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "Anatomical Engine - 105km Cavern Network", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexShaderSource, NULL); glCompileShader(vs);
    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentShaderSource, NULL); glCompileShader(fs);
    unsigned int shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs); glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);

    std::vector<float> quadVertices = { -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f };
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO); glGenBuffers(1, &VBO);
    glBindVertexArray(VAO); glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, quadVertices.size() * sizeof(float), quadVertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    std::vector<char> titleBuffer(256);

        while (!glfwWindowShouldClose(window)) {
        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, true);
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

        // Обычная скорость: 1.5 км/с | Форсаж (Shift): 15 км/с
        float baseSpeed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ? 15000.0f : 1500.0f;
        float speed = baseSpeed * deltaTime;

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { 
            camPos.x += fwd.x * speed; 
            camPos.y += fwd.y * speed; 
            camPos.z += fwd.z * speed; 
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { 
            camPos.x -= fwd.x * speed; 
            camPos.y -= fwd.y * speed; 
            camPos.z -= fwd.z * speed; 
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { 
            camPos.x -= right.x * speed; 
            camPos.y -= right.y * speed; 
            camPos.z -= right.z * speed; 
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { 
            camPos.x += right.x * speed; 
            camPos.y += right.y * speed; 
            camPos.z += right.z * speed; 
        }

        int fps = deltaTime > 0.0f ? static_cast<int>(1.0f / deltaTime) : 0;
        snprintf(titleBuffer.data(), titleBuffer.size(), 
                 "Anatomical Engine | Depth: %.2f / 105.00 km | FPS: %d | [Shift: 15 km/s | LMB: Blast]", 
                 camPos.z / 1000.0f, fps);
        glfwSetWindowTitle(window, titleBuffer.data());

        int fbWidth, fbHeight;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);

        glUseProgram(shaderProgram);
        glUniform2f(glGetUniformLocation(shaderProgram, "uResolution"), (float)fbWidth, (float)fbHeight);
        glUniform3f(glGetUniformLocation(shaderProgram, "uCamPos"), camPos.x, camPos.y, camPos.z);
        glUniform3f(glGetUniformLocation(shaderProgram, "uCamForward"), fwd.x, fwd.y, fwd.z);
        glUniform3f(glGetUniformLocation(shaderProgram, "uCamRight"), right.x, right.y, right.z);
        glUniform3f(glGetUniformLocation(shaderProgram, "uCamUp"), up.x, up.y, up.z);
        
        glUniform4f(glGetUniformLocation(shaderProgram, "uCrater0"), crater0.x, crater0.y, crater0.z, crater0.w);
        glUniform4f(glGetUniformLocation(shaderProgram, "uCrater1"), crater1.x, crater1.y, crater1.z, crater1.w);
        glUniform4f(glGetUniformLocation(shaderProgram, "uCrater2"), crater2.x, crater2.y, crater2.z, crater2.w);
        glUniform4f(glGetUniformLocation(shaderProgram, "uCrater3"), crater3.x, crater3.y, crater3.z, crater3.w);
        glUniform1f(glGetUniformLocation(shaderProgram, "uTime"), currentFrame);

        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}
