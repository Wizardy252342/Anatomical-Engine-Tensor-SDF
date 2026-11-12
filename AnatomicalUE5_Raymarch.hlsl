// ============================================================================
// Anatomical Engine - Unreal Engine 5 Custom Material Node (HLSL)
// ============================================================================

struct FAnatomicalHLSL
{
    // Ортогональная матрица SO(3) для непериодических октав шума Эшера
    float3 RotateSO3(float3 v)
    {
        float3x3 rot = float3x3(
             0.36,  0.48, -0.80,
            -0.80,  0.60,  0.00,
             0.48,  0.64,  0.60
        );
        return mul(rot, v);
    }

    // Апериодический шум Эшера с функциональным затуханием у поверхности
    float EscherTensorNoise(float3 p)
    {
        float3 q = p * 0.032;
        float n1 = sin(q.x) * cos(q.y) * sin(q.z);
        q = RotateSO3(q) * 2.13;
        float n2 = sin(q.x) * cos(q.y) * sin(q.z) * 0.45;
        return n1 + n2;
    }

    // Физический спектр излучения абсолютно чёрного тела (Кельвины -> RGB)
    float3 BlackbodyRadiation(float tempK)
    {
        float t = saturate((tempK - 700.0) / 2500.0);
        float3 cold = float3(0.45, 0.04, 0.01);
        float3 warm = float3(1.00, 0.38, 0.04);
        float3 hot  = float3(1.00, 0.96, 0.85);
        if (t < 0.5) return lerp(cold, warm, t * 2.0);
        return lerp(warm, hot, (t - 0.5) * 2.0);
    }

    // Оценка поля расстояний (в метрах, система координат Core: Y-вверх)
    float EvaluateSDF(
        float3 p, 
        float planetRadius, 
        float caveScale, 
        float4 craterData, 
        float timeVal,
        out float3 outMagField, 
        out float outTempK
    )
    {
        float sdfPlanet = length(p + float3(0.0, planetRadius, 0.0)) - planetRadius;
        float3 g = p * caveScale;
        float gyroid = abs(dot(sin(g), cos(g.yzx))) / caveScale;
        
        float2 shaftOffset = float2(
            sin(p.z * 0.0002) * 1800.0, 
            cos(p.z * 0.00015) * 1200.0
        );
        float mainShaft = length(p.xy - shaftOffset) - 950.0;
        float sdfCaves = min(mainShaft, gyroid - 420.0);
        float zBounds = max(-p.z, p.z - 105000.0);
        sdfCaves = max(sdfCaves, zBounds);

        float baseSDF = max(sdfPlanet, -sdfCaves);
        float damping = exp(-0.00008 * baseSDF * baseSDF);
        float microDetails = EscherTensorNoise(p) * 36.0 * damping;

        // Оператор возмущения кратера (256-байтный дельта-пакет)
        float craterDelta = 0.0;
        outTempK = clamp((p.z / 105000.0) * 950.0, 0.0, 950.0);

        if (craterData.w > 0.5)
        {
            float3 diff = (p - craterData.xyz) * float3(1.15, 0.92, 1.08);
            float dist = length(diff);
            if (dist < 480.0)
            {
                float rNorm = dist / 115.0;
                float bowl = 230.0 * exp(-0.5 * rNorm * rNorm);
                float rim = -45.0 * exp(-0.5 * pow((dist - 240.0) / 55.0, 2.0));
                craterDelta = bowl + rim;
                outTempK += 2600.0 * exp(-0.5 * pow(dist / 150.0, 2.0));
            }
        }

        outMagField = float3(
            sin(p.y * 0.004 + p.z * 0.001 + timeVal * 0.4),
            cos(p.x * 0.004 - p.z * 0.001),
            sin(p.z * 0.002)
        );

        return baseSDF + microDetails + craterDelta;
    }

    float3 CalculateNormal(float3 p, float planetRadius, float caveScale, float4 craterData, float timeVal)
    {
        float2 e = float2(0.35, 0.0);
        float3 m; float temp;
        float d = EvaluateSDF(p, planetRadius, caveScale, craterData, timeVal, m, temp);
        float dx = EvaluateSDF(p + e.xyy, planetRadius, caveScale, craterData, timeVal, m, temp) - d;
        float dy = EvaluateSDF(p + e.yxy, planetRadius, caveScale, craterData, timeVal, m, temp) - d;
        float dz = EvaluateSDF(p + e.yyx, planetRadius, caveScale, craterData, timeVal, m, temp) - d;
        return normalize(float3(dx, dy, dz));
    }

    float CalculateAO(float3 p, float3 n, float planetRadius, float caveScale, float4 craterData, float timeVal)
    {
        float occ = 0.0;
        float sca = 1.0;
        float3 m; float temp;
        for (int i = 1; i <= 3; i++)
        {
            float h = 10.0 * float(i);
            float d = EvaluateSDF(p + h * n, planetRadius, caveScale, craterData, timeVal, m, temp);
            occ += (h - d) * sca;
            sca *= 0.6;
        }
        return clamp(1.0 - 0.025 * occ, 0.2, 1.0);
    }
};

// --- Точка входа для UE5 Custom Node ---
// Перевод координат из UE5 (см, Z-вверх) в AnatomicalCore (метры, Y-вверх)
float3 camMeters = float3(CameraPosition.x * 0.01, CameraPosition.z * 0.01, CameraPosition.y * 0.01);
float3 worldMeters = float3(WorldPosition.x * 0.01, WorldPosition.z * 0.01, WorldPosition.y * 0.01);
float3 rayDir = normalize(worldMeters - camMeters);

FAnatomicalHLSL Core;
float t = 0.0;
float3 p = camMeters;
float3 magField = float3(0.0, 0.0, 0.0);
float tempK = 0.0;
float d = 1000.0;

// Адаптивная трассировка луча (80 шагов)
for (int i = 0; i < 80; i++)
{
    p = camMeters + rayDir * t;
    d = Core.EvaluateSDF(p, PlanetRadiusMeters, CaveScale, CraterPosRadius, Time, magField, tempK);
    float stepScale = (d > 150.0) ? 1.15 : 0.80;
    t += d * stepScale;
    if (d < 0.25 || t > 120000.0) break;
}

float3 fogColor = float3(0.03, 0.06, 0.12);
if (d < 0.25)
{
    float3 normal = Core.CalculateNormal(p, PlanetRadiusMeters, CaveScale, CraterPosRadius, Time);
    float ao = Core.CalculateAO(p, normal, PlanetRadiusMeters, CaveScale, CraterPosRadius, Time);
    float3 lightDir = normalize((camMeters + rayDir * 800.0) - p);
    float diff = max(0.18, dot(normal, lightDir));
    float fog = exp(-t * 0.000045);
    float magLine = pow(abs(sin(magField.x * 3.0 + magField.y * 3.0)), 10.0);

    float3 rockCol = float3(0.44, 0.42, 0.47) * diff * ao;
    float3 magCol = float3(0.0, 0.72, 1.0) * magLine;
    float3 thermalGlow = float3(0.0, 0.0, 0.0);

    if (tempK > 700.0)
    {
        float intensity = clamp((tempK - 700.0) / 1400.0, 0.0, 1.8);
        thermalGlow = Core.BlackbodyRadiation(tempK) * intensity;
    }

    float3 finalRGB = lerp(fogColor, rockCol + magCol + thermalGlow, fog);
    return float4(finalRGB, 1.0);
}

return float4(fogColor, 0.0);
