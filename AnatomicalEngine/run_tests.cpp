#include "AnatomicalCore.hpp"
#include <iostream>
#include <cmath>
#include <limits>
#include <cassert>

class TestSphereWorld : public Anatomical::ISpatialWorld {
public:
    float EvaluateBaseSDF(const Anatomical::Vec3& p) const override {
        return Anatomical::SDF::Sphere(p, { 0.0f, 0.0f, 0.0f }, 1000.0f);
    }
};

int main() {
    std::cout << "=== Anatomical Engine v8.1: Automated Safety & Integrity Suite ===\n";

    // Тест 1: Защита от NaN, Inf и нулевого деления
    Anatomical::SpatialHashGrid grid(0.0f);
    Anatomical::PerturbationOperator badOp;
    badOp.position = { std::numeric_limits<float>::quiet_NaN(), 100.0f, 200.0f };
    assert(!grid.InsertOperator(badOp) && "Failed to reject NaN operator!");

    Anatomical::Vec3 zeroDiv = Anatomical::Vec3{ 10.0f, 20.0f, 30.0f } / 0.0f;
    assert(zeroDiv.IsValid() && "Zero division produced NaN/Inf!");
    std::cout << "[PASS] Test 1: NaN/Inf & Zero-Division Protection\n";

    // Тест 2: Бесшовная вставка на границе ячеек (Halo Insertion)
    Anatomical::SpatialHashGrid worldGrid(500.0f);
    Anatomical::PerturbationOperator borderOp;
    borderOp.position = { 500.0f, 500.0f, 500.0f };
    borderOp.timestamp = 1.0f;
    assert(worldGrid.InsertOperator(borderOp));

    float deltaLeft = worldGrid.EvaluateDeltaAt({ 499.0f, 500.0f, 500.0f });
    float deltaRight = worldGrid.EvaluateDeltaAt({ 501.0f, 500.0f, 500.0f });
    assert(std::abs(deltaLeft - deltaRight) < 5.0f && "Seam detected across cell boundary!");
    std::cout << "[PASS] Test 2: Seamless Cross-Cell Halo Insertion (Delta diff: "
              << std::abs(deltaLeft - deltaRight) << " m)\n";

    // Тест 3: Контрольная сумма и защита 256-байтного сетевого пакета
    Anatomical::ESNPacket256 pkt;
    pkt.senderId = 42;
    pkt.opType = 1;
    pkt.posX = 500.0f;
    pkt.Seal();
    assert(pkt.IsValid() && "Valid sealed packet rejected!");

    pkt.posX = 9999.0f;
    assert(!pkt.IsValid() && "Corrupted network packet was not detected!");
    std::cout << "[PASS] Test 3: ESNPacket256 FNV-1a Checksum Tamper Detection\n";

    // Тест 4: Трассировка лучей и коллизии твёрдого тела
    TestSphereWorld sphereWorld;
    Anatomical::SpatialHashGrid cleanGrid(500.0f);
    auto hit = Anatomical::FieldPhysicsSolver::RayCast(
        sphereWorld, cleanGrid, { 0.0f, 0.0f, 2000.0f }, { 0.0f, 0.0f, -1.0f }
    );
    assert(hit.hit && std::abs(hit.distance - 1000.0f) < 1.0f && "RayCast accuracy error!");
    std::cout << "[PASS] Test 4: Analytical RayCast Accuracy (Hit at " << hit.distance << " m)\n";

    std::cout << "=== ALL INTEGRITY TESTS PASSED SUCCESSFULLY ===\n";
    return 0;
}
