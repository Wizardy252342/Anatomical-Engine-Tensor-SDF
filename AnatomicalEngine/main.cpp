#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include <vector>

// Параметры окна и управления
const unsigned int SCR_WIDTH = 1280;
const unsigned int SCR_HEIGHT = 720;

// Координаты и углы обзора камеры (Наблюдателя)
float camPos[3] = { 399.0f, 108.0f, 1000.0f };
float yaw = 0.0f, pitch = 0.0f;
float lastX = SCR_WIDTH / 2.0f, lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;
float deltaTime = 0.0f, lastFrame = 0.0f;

// Координаты 64-байтного оператора разрушения (Кратер Δ)
float craterPos[3] = { 0.0f, 0.0f, -99999.0f };
float craterActive = 0.0f;

// 1. Вершинный шейдер (Создает полноэкранный холст для Ray Marching)
const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
out vec2 uv;
void main() {
    uv = aPos;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// 2. Фрагментный шейдер (Ядро Аналитической Реальности на GPU)
const char* fragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
in vec2 uv;

uniform vec2 uResolution;
uniform vec3 uCamPos;
uniform vec3 uCamForward;
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform vec3 uCraterPos;
uniform float uCraterActive;
uniform float uTime;

// Детерминированная матрица Эшера (Микро-детализация без текстур)
float EscherNoise(vec3 p) {
    float s = 0.07;
    float n1 = sin(p.x * s) * cos(p.y * s) * sin(p.z * s);
    float n2 = cos(p.x * s * 2.3) * sin(p.z * s * 2.3) * 0.5;
    return n1 + n2;
}

// Глобальный SDF + Пещера + Оператор Δ + Поля Максвелла
float GetDistanceAndPhysics(vec3 p, out vec3 outMagField, out float outCraterGlow) {
    float sdfPlanet = length(p) - 15000.0;
    
    // Аналитический сплайн 100-километровой пещеры
    float tunnelX = sin(p.z * 0.0015) * 400.0;
    float tunnelY = cos(p.z * 0.0012) * 300.0;
    float caveRadius = 290.0 + sin(p.z * 0.004) * 70.0;
    float sdfCave = length(p.xy - vec2(tunnelX, tunnelY)) - caveRadius;
    
    // Топологическая инверсия знака
    float baseSDF = max(sdfPlanet, -sdfCave);
    
    // Функциональное затухание шума Эшера (активируется только у стен)
    float damping = exp(-0.0005 * baseSDF * baseSDF);
    float microDetails = EscherNoise(p) * 14.0 * damping;
    float combinedSDF = baseSDF + microDetails;
    
    // Внедрение 64-байтного Векторного Оператора Возмущения Δ
    float deltaDestruction = 0.0;
    outCraterGlow = 0.0;
    if (uCraterActive > 0.5) {
        float distToImpact = length(p - uCraterPos);
        if (distToImpact < 220.0) {
            deltaDestruction = 110.0 * exp(-(distToImpact * distToImpact) / (2.0 * 60.0 * 60.0));
            outCraterGlow = exp(-(distToImpact * distToImpact) / (2.0 * 75.0 * 75.0));
        }
    }
    
    // Аналитическое магнитное поле Максвелла (пульсирует в реальном времени)
    outMagField = vec3(
        sin(p.y * 0.02 + p.z * 0.005 + uTime * 0.5),
        cos(p.x * 0.02 - p.z * 0.005),
        sin(p.z * 0.01)
    );
    
    return combinedSDF + deltaDestruction;
}

vec3 CalculateNormal(vec3 p) {
    vec2 e = vec2(0.05, 0.0);
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
    
    // Цикл Ray Marching на ядрах GPU
    for (int i = 0; i < 100; i++) {
        p = uCamPos + rayDir * t;
        d = GetDistanceAndPhysics(p, magField, craterGlow);
        t += d * 0.85;
        if (d < 0.05 || t > 25000.0) break;
    }
    
    vec3 col = vec3(0.06, 0.08, 0.15); // Атмосферный туман глубины
    if (d < 0.05) {
        vec3 normal = CalculateNormal(p);
        vec3 lightDir = normalize((uCamPos + uCamForward * 250.0) - p); // Прожектор корабля
        float diff = max(0.18, dot(normal, lightDir));
        float fog = exp(-t * 0.00035);
        
        float magLine = pow(abs(sin(magField.x * 4.0 + magField.y * 4.0)), 8.0);
        
        vec3 rockCol = vec3(0.45, 0.43, 0.47) * diff;
        vec3 magCol = vec3(0.0, 0.65, 0.98) * magLine;
        vec3 craterCol = vec3(0.95, 0.35, 0.05) * craterGlow;
        
        col = mix(col, rockCol + magCol + craterCol, fog);
    }
    
    FragColor = vec4(col, 1.0);
}
)";

// Математические помощники для векторов камеры
void Cross(const float a[3], const float b[3], float res[3]) {
    res[0] = a[1]*b[2] - a[2]*b[1];
    res[1] = a[2]*b[0] - a[0]*b[2];
    res[2] = a[0]*b[1] - a[1]*b[0];
}
void Normalize(float v[3]) {
    float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 0) { v[0]/=l; v[1]/=l; v[2]/=l; }
}

// Обработка мыши (поворот камеры)
void mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);
    if (firstMouse) { lastX = xpos; lastY = ypos; firstMouse = false; }
    float xoffset = (xpos - lastX) * 0.15f;
    float yoffset = (lastY - ypos) * 0.15f;
    lastX = xpos; lastY = ypos;
    yaw += xoffset; pitch += yoffset;
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
}

// Обработка клика (выстрел 64-байтным оператором разрушения Δ)
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        float radYaw = yaw * 3.14159f / 180.0f;
        float radPitch = pitch * 3.14159f / 180.0f;
        float fwd[3] = {
            std::sin(radYaw) * std::cos(radPitch),
            std::sin(radPitch),
            std::cos(radYaw) * std::cos(radPitch)
        };
        // Проецируем точку взрыва на 600 метров перед носом корабля
        craterPos[0] = camPos[0] + fwd[0] * 600.0f;
        craterPos[1] = camPos[1] + fwd[1] * 600.0f;
        craterPos[2] = camPos[2] + fwd[2] * 600.0f;
        craterActive = 1.0f;
        std::cout << "[ESN Network Packet] 64-byte Perturbation Operator dispatched at (" 
                  << craterPos[0] << ", " << craterPos[1] << ", " << craterPos[2] << ")\n";
    }
}

int main() {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "Anatomical Engine - Tensor SDF Sandbox (WASD + Mouse + LMB to Blast)", NULL, NULL);
    if (window == NULL) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    // Компиляция шейдеров
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

    glDeleteShader(vs);
    glDeleteShader(fs);

    // Полноэкранный прямоугольник
    float quadVertices[] = { 
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
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

 int fbWidth, fbHeight;
 glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
glViewport(0, 0, fbWidth, fbHeight);

    // Главный цикл симуляции (60+ FPS)
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) 
            glfwSetWindowShouldClose(window, true);

        float radYaw = yaw * 3.14159f / 180.0f;
        float radPitch = pitch * 3.14159f / 180.0f;
        float fwd[3] = { 
            std::sin(radYaw) * std::cos(radPitch), 
            std::sin(radPitch), 
            std::cos(radYaw) * std::cos(radPitch) 
        };
        Normalize(fwd);
        
        float worldUp[3] = { 0.0f, 1.0f, 0.0f };
        float right[3]; 
        Cross(worldUp, fwd, right); 
        Normalize(right);
        
        float up[3]; 
        Cross(fwd, right, up); 
        Normalize(up);


        // Скорость полета корабля (Shift — форсаж)
        float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 1800.0f : 500.0f) * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { for(int i = 0; i < 3; i++) camPos[i] += fwd[i] * speed; }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { for(int i = 0; i < 3; i++) camPos[i] -= fwd[i] * speed; }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { for(int i = 0; i < 3; i++) camPos[i] -= right[i] * speed; }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { for(int i = 0; i < 3; i++) camPos[i] += right[i] * speed; }

        // Передача всех параметров в GPU шейдер
        glUseProgram(shaderProgram);
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);
        glUniform2f(glGetUniformLocation(shaderProgram, "uResolution"), (float)fbWidth, (float)fbHeight);
        glUniform3fv(glGetUniformLocation(shaderProgram, "uCamPos"), 1, camPos);
        glUniform3fv(glGetUniformLocation(shaderProgram, "uCamForward"), 1, fwd);
        glUniform3fv(glGetUniformLocation(shaderProgram, "uCamRight"), 1, right);
        glUniform3fv(glGetUniformLocation(shaderProgram, "uCamUp"), 1, up);
        glUniform3fv(glGetUniformLocation(shaderProgram, "uCraterPos"), 1, craterPos);
        glUniform1f(glGetUniformLocation(shaderProgram, "uCraterActive"), craterActive);
        glUniform1f(glGetUniformLocation(shaderProgram, "uTime"), currentFrame);

        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteProgram(shaderProgram);

    glfwTerminate();
    return 0;
}
