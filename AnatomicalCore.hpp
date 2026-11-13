#pragma once

#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>

namespace Anatomical {

// ============================================================================
// 1. БАЗОВАЯ ЛИНЕЙНАЯ АЛГЕБРА И МАТРИЧНЫЕ ОПЕРАТОРЫ
// ============================================================================

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3 operator+(const Vec3& r) const { return { x + r.x, y + r.y, z + r.z }; }
    Vec3 operator-(const Vec3& r) const { return { x - r.x, y - r.y, z - r.z }; }
    Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
    Vec3 operator/(float s) const { return { x / s, y / s, z / s }; }

    float Dot(const Vec3& r) const { return x * r.x + y * r.y + z * r.z; }
    Vec3 Cross(const Vec3& r) const {
        return { y * r.z - z * r.y, z * r.x - x * r.z, x * r.y - y * r.x };
    }
    float LengthSq() const { return x * x + y * y + z * z; }
    float Length() const { return std::sqrt(LengthSq()); }
    Vec3 Normalized() const {
        float l = Length();
        return (l > 1e-6f) ? (*this / l) : Vec3{ 0.0f, 0.0f, 0.0f };
    }
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Mat3 {
    float m00, m01, m02;
    float m10, m11, m12;
    float m20, m21, m22;

    Vec3 Multiply(const Vec3& v) const {
        return {
            m00 * v.x + m01 * v.y + m02 * v.z,
            m10 * v.x + m11 * v.y + m12 * v.z,
            m20 * v.x + m21 * v.y + m22 * v.z
        };
    }

    // Ортогональная матрица SO(3) для апериодических октав деталей
    static Mat3 OrthogonalEscherSO3() {
        return {
             0.36f,  0.48f, -0.80f,
            -0.80f,  0.60f,  0.00f,
             0.48f,  0.64f,  0.60f
        };
    }
};

// ============================================================================
// 2. АНАЛИТИЧЕСКИЕ ОПЕРАТОРЫ ПОЛЯ РАССТОЯНИЙ (SDF PRIMITIVES & CSG)
// ============================================================================

namespace SDF {
    inline float Sphere(const Vec3& p, const Vec3& center, float radius) {
        return (p - center).Length() - radius;
    }

    inline float Box(const Vec3& p, const Vec3& center, const Vec3& halfExtents) {
        Vec3 q = {
            std::abs(p.x - center.x) - halfExtents.x,
            std::abs(p.y - center.y) - halfExtents.y,
            std::abs(p.z - center.z) - halfExtents.z
        };
        Vec3 qMax = { std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f) };
        float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
        return qMax.Length() + inside;
    }

    inline float Gyroid(const Vec3& p, float scale) {
        Vec3 g = p * scale;
        float d = std::sin(g.x) * std::cos(g.y) +
                  std::sin(g.y) * std::cos(g.z) +
                  std::sin(g.z) * std::cos(g.x);
        return std::abs(d) / scale;
    }

    // Гладкое объединение и вычитание (Smooth CSG)
    inline float SmoothUnion(float d1, float d2, float k) {
        float h = std::max(k - std::abs(d1 - d2), 0.0f) / k;
        return std::min(d1, d2) - h * h * k * 0.25f;
    }

    inline float SmoothSubtract(float d1, float d2, float k) {
        float h = std::max(k - std::abs(-d1 - d2), 0.0f) / k;
        return std::max(d1, -d2) + h * h * k * 0.25f;
    }

    // Апериодический шум Эшера с затуханием от границы поверхности (|SDF| -> 0)
    inline float EscherMicroDetail(const Vec3& p, float baseSDF, float amplitude, float freq) {
        float damping = std::exp(-0.00008f * baseSDF * baseSDF);
        if (damping < 0.001f) return 0.0f;

        Mat3 rot = Mat3::OrthogonalEscherSO3();
        Vec3 q = p * freq;
        float n1 = std::sin(q.x) * std::cos(q.y) * std::sin(q.z);
        q = rot.Multiply(q) * 2.13f;
        float n2 = std::sin(q.x) * std::cos(q.y) * std::sin(q.z) * 0.45f;
        return (n1 + n2) * amplitude * damping;
    }
}

// ============================================================================
// 3. ПРОСТРАНСТВЕННАЯ ХЕШ-ТАБЛИЦА O(1) ДЛЯ ДИНАМИЧЕСКИХ ОПЕРАТОРОВ ДЕЛЬТ
// ============================================================================

struct PerturbationOperator {
    Vec3 position;
    float radius;
    float intensity;
    float timestamp;
    float temperatureK;
    Vec3 anisotropy;
};

class SpatialHashGrid {
public:
    static constexpr int BUCKETS = 16384;
    static constexpr int SLOTS_PER_BUCKET = 4;

    explicit SpatialHashGrid(float cellSize = 500.0f)
        : m_cellSize(cellSize), m_gpuBuffer(BUCKETS * SLOTS_PER_BUCKET, { 0.0f, 0.0f, 0.0f, 0.0f }) {}

    void InsertOperator(const PerturbationOperator& op) {
        int cx = static_cast<int>(std::floor(op.position.x / m_cellSize));
        int cy = static_cast<int>(std::floor(op.position.y / m_cellSize));
        int cz = static_cast<int>(std::floor(op.position.z / m_cellSize));
        uint32_t bucket = ComputeHash(cx, cy, cz);

        int baseIdx = static_cast<int>(bucket) * SLOTS_PER_BUCKET;
        for (int i = SLOTS_PER_BUCKET - 1; i > 0; --i) {
            m_gpuBuffer.at(baseIdx + i) = m_gpuBuffer.at(baseIdx + i - 1);
        }
        m_gpuBuffer.at(baseIdx) = { op.position.x, op.position.y, op.position.z, op.timestamp + 1.0f };
        m_totalCount++;
        m_dirty = true;
    }

    float EvaluateDeltaAt(const Vec3& p, float defaultRadius = 115.0f, float defaultDepth = 230.0f) const {
        int cx = static_cast<int>(std::floor(p.x / m_cellSize));
        int cy = static_cast<int>(std::floor(p.y / m_cellSize));
        int cz = static_cast<int>(std::floor(p.z / m_cellSize));
        uint32_t bucket = ComputeHash(cx, cy, cz);
        int baseIdx = static_cast<int>(bucket) * SLOTS_PER_BUCKET;

        float totalDelta = 0.0f;
        float maxRange = defaultRadius * 4.0f;
        float maxRangeSq = maxRange * maxRange;

        for (int i = 0; i < SLOTS_PER_BUCKET; ++i) {
            const Vec4& c = m_gpuBuffer.at(baseIdx + i);
            if (c.w > 0.5f) {
                Vec3 diff = { p.x - c.x, p.y - c.y, p.z - c.z };
                float distSq = diff.LengthSq();
                if (distSq < maxRangeSq) {
                    totalDelta += defaultDepth * std::exp(-distSq / (2.0f * defaultRadius * defaultRadius));
                }
            }
        }
        return totalDelta;
    }

    void Clear() {
        std::fill(m_gpuBuffer.begin(), m_gpuBuffer.end(), Vec4{ 0.0f, 0.0f, 0.0f, 0.0f });
        m_totalCount = 0;
        m_dirty = true;
    }

    // Прямой доступ к выровненному буферу для загрузки в GPU (GL_TEXTURE_BUFFER / SSBO)
    const Vec4* GetRawBuffer() const { return m_gpuBuffer.data(); }
    size_t GetBufferByteSize() const { return m_gpuBuffer.size() * sizeof(Vec4); }
    int GetActiveCount() const { return m_totalCount; }
    bool IsDirty() const { return m_dirty; }
    void ClearDirtyFlag() { m_dirty = false; }

private:
    float m_cellSize;
    std::vector<Vec4> m_gpuBuffer;
    int m_totalCount = 0;
    bool m_dirty = true;

    static uint32_t ComputeHash(int ix, int iy, int iz) {
        uint32_t ux = static_cast<uint32_t>(ix + 100000);
        uint32_t uy = static_cast<uint32_t>(iy + 100000);
        uint32_t uz = static_cast<uint32_t>(iz + 100000);
        uint32_t h = (ux * 73856093u) ^ (uy * 19349663u) ^ (uz * 83492791u);
        return h % BUCKETS;
    }
};

// ============================================================================
// 4. АБСТРАКТНЫЙ ИНТЕРФЕЙС МИРА И ФИЗИЧЕСКИЙ / БАЛЛИСТИЧЕСКИЙ РЕШАТЕЛЬ
// ============================================================================

class ISpatialWorld {
public:
    virtual ~ISpatialWorld() = default;
    virtual float EvaluateBaseSDF(const Vec3& p) const = 0;
};

struct RayHitResult {
    bool hit = false;
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
    int steps = 0;
};

class FieldPhysicsSolver {
public:
    static float EvaluateTotalSDF(const ISpatialWorld& world, const SpatialHashGrid& deltas, const Vec3& p) {
        return world.EvaluateBaseSDF(p) + deltas.EvaluateDeltaAt(p);
    }

    static Vec3 ComputeNormal(const ISpatialWorld& world, const SpatialHashGrid& deltas, const Vec3& p, float eps = 0.5f) {
        float d = EvaluateTotalSDF(world, deltas, p);
        Vec3 grad = {
            EvaluateTotalSDF(world, deltas, { p.x + eps, p.y, p.z }) - d,
            EvaluateTotalSDF(world, deltas, { p.x, p.y + eps, p.z }) - d,
            EvaluateTotalSDF(world, deltas, { p.x, p.y, p.z + eps }) - d
        };
        return grad.Normalized();
    }

    // Разрешение коллизий твёрдого тела со стенками поля без треугольников
    static bool ResolveSphereCollision(
        const ISpatialWorld& world,
        const SpatialHashGrid& deltas,
        Vec3& position,
        Vec3& velocity,
        float hullRadius,
        float restitution = 0.3f
    ) {
        float dist = EvaluateTotalSDF(world, deltas, position);
        if (dist < hullRadius) {
            Vec3 normal = ComputeNormal(world, deltas, position);
            float penetration = hullRadius - dist;
            position = position + normal * penetration;

            float vDotN = velocity.Dot(normal);
            if (vDotN < 0.0f) {
                velocity = velocity - normal * ((1.0f + restitution) * vDotN);
            }
            return true;
        }
        return false;
    }

    // Серверная / ИИ трассировка луча без использования GPU (для баллистики и LiDAR)
    static RayHitResult RayCast(
        const ISpatialWorld& world,
        const SpatialHashGrid& deltas,
        const Vec3& origin,
        const Vec3& direction,
        float maxDistance = 50000.0f,
        int maxSteps = 128
    ) {
                RayHitResult result;
        Vec3 dir = direction.Normalized();
        float t = 0.0f;

        for (int i = 0; i < maxSteps; ++i) {
            Vec3 p = origin + dir * t;
            float d = EvaluateTotalSDF(world, deltas, p);
            result.steps = i + 1;

            if (d < 0.25f) {
                result.hit = true;
                result.point = p;
                result.distance = t;
                result.normal = ComputeNormal(world, deltas, p);
                return result;
            }
            t += d * 0.9f;
            if (t > maxDistance) break;
        }
        return result;
    }
};

// ============================================================================
// 5. СЕТЕВОЙ ПРОТОКОЛ ВЫСОКОЙ ТОЧНОСТИ (256-BYTE ENTANGLED STATE PACKET)
// ============================================================================

struct alignas(64) ESNPacket256 {
    uint32_t magic = 0x45534E35; // 'ESN5'
    uint32_t senderId = 0;
    uint32_t opType = 0;
    uint32_t seed = 0;
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    float temperatureK = 0.0f;

    // 16 коэффициентов матрицы тензорной деформации (64 байта)
    float t00 = 1.0f, t01 = 0.0f, t02 = 0.0f, t03 = 0.0f;
    float t10 = 0.0f, t11 = 1.0f, t12 = 0.0f, t13 = 0.0f;
    float t20 = 0.0f, t21 = 0.0f, t22 = 1.0f, t23 = 0.0f;
    float t30 = 0.0f, t31 = 0.0f, t32 = 0.0f, t33 = 1.0f;

    // Расширенный резерв тензорных гармоник (160 байт -> итого ровно 256 байт)
    uint64_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
    uint64_t r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0;
    uint64_t r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0;
    uint64_t r15 = 0, r16 = 0, r17 = 0, r18 = 0, r19 = 0;

    bool IsValid() const { return magic == 0x45534E35; }
};
static_assert(sizeof(ESNPacket256) == 256, "ESNPacket256 must be strictly 256 bytes");

// ============================================================================
// 6. ФРАКТАЛЬНЫЙ РОУТЕР СОСТОЯНИЙ (HIERARCHICAL MERKLE-HASH ROUTER)
//    Связывает ИИ-агентов в центре с распределёнными игровыми движками
// ============================================================================

class HierarchicalHashRouter {
public:
    static constexpr int LEAF_BUCKETS = SpatialHashGrid::BUCKETS; // 16384
    static constexpr int REGION_COUNT = 256;                      // 256 регионов (по 64 листа)
    static constexpr int LEAVES_PER_REGION = LEAF_BUCKETS / REGION_COUNT;

    HierarchicalHashRouter()
        : m_leafHashes(LEAF_BUCKETS, 0ULL), m_regionHashes(REGION_COUNT, 0ULL) {}

    // Пересчитывает иерархическое дерево хешей на основе буфера SpatialHashGrid
    void RebuildFromGrid(const SpatialHashGrid& grid) {
        const Vec4* buf = grid.GetRawBuffer();
        uint64_t newRoot = 0ULL;

        for (int r = 0; r < REGION_COUNT; ++r) {
            uint64_t regHash = 0xCBF29CE484222325ULL; // FNV-1a offset basis
            int startLeaf = r * LEAVES_PER_REGION;

            for (int l = 0; l < LEAVES_PER_REGION; ++l) {
                int leafIdx = startLeaf + l;
                int baseSlot = leafIdx * SpatialHashGrid::SLOTS_PER_BUCKET;
                uint64_t leafHash = 0ULL;

                for (int s = 0; s < SpatialHashGrid::SLOTS_PER_BUCKET; ++s) {
                    const Vec4& op = buf[baseSlot + s];
                    if (op.w > 0.5f) {
                        uint64_t h = HashVec4(op);
                        leafHash ^= (h + 0x9E3779B97F4A7C15ULL + (leafHash << 6) + (leafHash >> 2));
                    }
                }

                m_leafHashes.at(leafIdx) = leafHash;
                regHash ^= leafHash;
                regHash *= 0x100000001B3ULL;
            }

            m_regionHashes.at(r) = regHash;
            newRoot ^= (regHash + static_cast<uint64_t>(r) * 0x517CC1B727220A95ULL);
        }

        m_globalRootHash = newRoot;
    }

    // Единый 64-битный хеш состояния всей планеты (0 байт трафика при совпадении)
    uint64_t GetGlobalRootHash() const { return m_globalRootHash; }

    // Быстрая проверка расхождений между ИИ-агентом и клиентским движком
    std::vector<int> FindDivergentRegions(const HierarchicalHashRouter& remoteRouter) const {
        std::vector<int> dirtyRegions;
        if (m_globalRootHash == remoteRouter.m_globalRootHash) {
            return dirtyRegions;
        }
        for (int r = 0; r < REGION_COUNT; ++r) {
            if (m_regionHashes.at(r) != remoteRouter.m_regionHashes.at(r)) {
                dirtyRegions.push_back(r);
            }
        }
        return dirtyRegions;
    }

    // Возвращает индексы конкретных 500-метровых ячеек внутри региона для точечной докачки
    std::vector<int> FindDivergentLeaves(int regionIndex, const HierarchicalHashRouter& remoteRouter) const {
        std::vector<int> dirtyLeaves;
        if (regionIndex < 0 || regionIndex >= REGION_COUNT) return dirtyLeaves;

        int startLeaf = regionIndex * LEAVES_PER_REGION;
        for (int l = 0; l < LEAVES_PER_REGION; ++l) {
            int idx = startLeaf + l;
            if (m_leafHashes.at(idx) != remoteRouter.m_leafHashes.at(idx)) {
                dirtyLeaves.push_back(idx);
            }
        }
        return dirtyLeaves;
    }

private:
    std::vector<uint64_t> m_leafHashes;
    std::vector<uint64_t> m_regionHashes;
    uint64_t m_globalRootHash = 0ULL;

    static uint64_t HashVec4(const Vec4& v) {
        uint32_t ux = 0, uy = 0, uz = 0, uw = 0;
        std::memcpy(&ux, &v.x, sizeof(uint32_t));
        std::memcpy(&uy, &v.y, sizeof(uint32_t));
        std::memcpy(&uz, &v.z, sizeof(uint32_t));
        std::memcpy(&uw, &v.w, sizeof(uint32_t));
        uint64_t a = (static_cast<uint64_t>(ux) << 32) | uy;
        uint64_t b = (static_cast<uint64_t>(uz) << 32) | uw;
        return a ^ (b * 0x9E3779B97F4A7C15ULL);
    }
};
} // namespace Anatomical
