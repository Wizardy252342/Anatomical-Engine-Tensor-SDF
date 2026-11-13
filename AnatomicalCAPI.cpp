#include "AnatomicalCore.hpp"
#include <cstdint>

#ifdef _WIN32
#define ANATOMICAL_EXPORT __declspec(dllexport)
#else
#define ANATOMICAL_EXPORT __attribute__((visibility("default")))
#endif

// Базовый аналитический мир для C-API и Python ИИ-агентов
class CAPIPlanetWorld : public Anatomical::ISpatialWorld {
public:
    float EvaluateBaseSDF(const Anatomical::Vec3& p) const override {
        float sdfPlanet = Anatomical::SDF::Sphere(p, { 0.0f, -15000000.0f, 0.0f }, 15000000.0f);
        float gyroid = Anatomical::SDF::Gyroid(p, 0.0006f) - 420.0f;
        return std::max(sdfPlanet, -gyroid);
    }
};

struct AnatomicalContext {
    Anatomical::SpatialHashGrid grid;
    Anatomical::HierarchicalHashRouter router;
    CAPIPlanetWorld world;

    AnatomicalContext(float cellSize) : grid(cellSize) {}
};

extern "C" {

// 1. Создание и удаление изолированного мира для ИИ-агентов
ANATOMICAL_EXPORT void* Anatomical_CreateContext(float cellSizeMeters) {
    return new AnatomicalContext(cellSizeMeters);
}

ANATOMICAL_EXPORT void Anatomical_DestroyContext(void* ctxPtr) {
    delete static_cast<AnatomicalContext*>(ctxPtr);
}

// 2. Добавление 256-байтного оператора кратера/разрушения и пересчет дерева хешей
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
    op.temperatureK = 2800.0f;
    op.anisotropy = { 1.15f, 0.92f, 1.08f };

    ctx->grid.InsertOperator(op);
    ctx->router.RebuildFromGrid(ctx->grid);
    return ctx->router.GetGlobalRootHash();
}

// 3. Получение корневого 64-битного хеша Меркла (0 байт трафика при совпадении)
ANATOMICAL_EXPORT uint64_t Anatomical_GetGlobalRootHash(void* ctxPtr) {
    if (!ctxPtr) return 0ULL;
    auto* ctx = static_cast<AnatomicalContext*>(ctxPtr);
    return ctx->router.GetGlobalRootHash();
}

// 4. Мгновенная оценка SDF (дистанции до ближайшей скалы) для навигации ИИ-агента
ANATOMICAL_EXPORT float Anatomical_EvaluateSDF(void* ctxPtr, float x, float y, float z) {
    if (!ctxPtr) return 0.0f;
    auto* ctx = static_cast<AnatomicalContext*>(ctxPtr);
    return Anatomical::FieldPhysicsSolver::EvaluateTotalSDF(ctx->world, ctx->grid, { x, y, z });
}

// 5. Серверный LiDAR / баллистический луч для ИИ-агентов (без GPU)
ANATOMICAL_EXPORT int Anatomical_RayCast(
    void* ctxPtr,
    float ox, float oy, float oz,
    float dx, float dy, float dz,
    float maxDist,
    float* outHitX, float* outHitY, float* outHitZ, float* outDist
) {
    if (!ctxPtr) return 0;
    auto* ctx = static_cast<AnatomicalContext*>(ctxPtr);

    Anatomical::RayHitResult res = Anatomical::FieldPhysicsSolver::RayCast(
        ctx->world, ctx->grid, { ox, oy, oz }, { dx, dy, dz }, maxDist, 128
    );

    if (res.hit) {
        if (outHitX) *outHitX = res.point.x;
        if (outHitY) *outHitY = res.point.y;
        if (outHitZ) *outHitZ = res.point.z;
        if (outDist) *outDist = res.distance;
        return 1;
    }
    return 0;
}

} // extern "C"
