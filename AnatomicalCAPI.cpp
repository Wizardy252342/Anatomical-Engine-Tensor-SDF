#include "AnatomicalCore.hpp"
#include <cstdint>

#ifdef _WIN32
#define ANATOMICAL_EXPORT __declspec(dllexport)
#else
#define ANATOMICAL_EXPORT __attribute__((visibility("default")))
#endif

class CAPIPlanetWorld : public Anatomical::ISpatialWorld {
public:
    float EvaluateBaseSDF(const Anatomical::Vec3& p) const override {
        float sdfPlanet = Anatomical::SDF::Sphere(p, { 0.0f, -15000000.0f, 0.0f }, 15000000.0f);
        float gyroid = Anatomical::SDF::Gyroid(p, 0.0006f) - 420.0f;
        float mainShaft = std::sqrt(
            std::pow(p.x - std::sin(p.z * 0.0002f) * 1800.0f, 2.0f) +
            std::pow(p.y - std::cos(p.z * 0.00015f) * 1200.0f, 2.0f)
        ) - 950.0f;
        float caves = std::min(mainShaft, gyroid);
        return std::max(sdfPlanet, -caves);
    }
};

struct AnatomicalContext {
    Anatomical::SpatialHashGrid grid;
    Anatomical::HierarchicalHashRouter router;
    CAPIPlanetWorld world;

    AnatomicalContext(float cellSize) : grid(cellSize) {}
};

extern "C" {

ANATOMICAL_EXPORT void* Anatomical_CreateContext(float cellSizeMeters) {
    return new AnatomicalContext(cellSizeMeters);
}

ANATOMICAL_EXPORT void Anatomical_DestroyContext(void* ctxPtr) {
    delete static_cast<AnatomicalContext*>(ctxPtr);
}

ANATOMICAL_EXPORT uint64_t Anatomical_InsertOperator(
    void* ctxPtr, float x, float y, float z, float radius, float intensity, float timestamp
) {
    if (!ctxPtr) return 0ULL;
    auto* ctx = static_cast<AnatomicalContext*>(ctxPtr);

    Anatomical::PerturbationOperator op;
    op.position = { x, y, z };
    op.radius = radius;
    op.intensity = intensity;
    op.timestamp = timestamp;

    ctx->grid.InsertOperator(op);
    ctx->router.RebuildFromGrid(ctx->grid);
    return ctx->router.GetGlobalRootHash();
}

ANATOMICAL_EXPORT uint64_t Anatomical_GetGlobalRootHash(void* ctxPtr) {
    if (!ctxPtr) return 0ULL;
    return static_cast<AnatomicalContext*>(ctxPtr)->router.GetGlobalRootHash();
}

ANATOMICAL_EXPORT float Anatomical_EvaluateSDF(void* ctxPtr, float x, float y, float z) {
    if (!ctxPtr) return 0.0f;
    auto* ctx = static_cast<AnatomicalContext*>(ctxPtr);
    return Anatomical::FieldPhysicsSolver::EvaluateTotalSDF(ctx->world, ctx->grid, { x, y, z });
}

// Пакетный расчёт SDF для тысяч точек за один вызов из Python
ANATOMICAL_EXPORT void Anatomical_EvaluateBatchSDF(
    void* ctxPtr, const float* xyzArray, float* outDistances, int count
) {
    if (!ctxPtr || !xyzArray || !outDistances || count <= 0) return;
    auto* ctx = static_cast<AnatomicalContext*>(ctxPtr);
    const auto* vecPoints = reinterpret_cast<const Anatomical::Vec3*>(xyzArray);
    Anatomical::FieldPhysicsSolver::EvaluateBatchSDF(
        ctx->world, ctx->grid, vecPoints, outDistances, static_cast<size_t>(count)
    );
}

// Пакетный LiDAR для автономных дронов (тысячи лучей за один вызов)
ANATOMICAL_EXPORT void Anatomical_RayCastBatch(
    void* ctxPtr,
    const float* originsXYZ,
    const float* dirsXYZ,
    float maxDistance,
    float* outDistances,
    uint8_t* outHits,
    int rayCount
) {
    if (!ctxPtr || !originsXYZ || !dirsXYZ || !outDistances || !outHits || rayCount <= 0) return;
    auto* ctx = static_cast<AnatomicalContext*>(ctxPtr);
    const auto* origins = reinterpret_cast<const Anatomical::Vec3*>(originsXYZ);
    const auto* dirs = reinterpret_cast<const Anatomical::Vec3*>(dirsXYZ);
    Anatomical::FieldPhysicsSolver::RayCastBatch(
        ctx->world, ctx->grid, origins, dirs, maxDistance,
        outDistances, outHits, static_cast<size_t>(rayCount)
    );
}

} // extern "C"
