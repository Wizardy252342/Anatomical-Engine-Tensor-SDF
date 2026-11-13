#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AnatomicalCore.hpp"
#include "AnatomicalUE5Component.generated.h"

// Пример базового аналитического мира для UE5 (Планета + Гироидные пещеры)
class FUE5AnalyticalPlanetWorld : public Anatomical::ISpatialWorld {
public:
    float PlanetRadiusMeters = 15000000.0f;
    float CaveScale = 0.0006f;
    float CaveWallThickness = 420.0f;

    virtual float EvaluateBaseSDF(const Anatomical::Vec3& p) const override {
        Anatomical::Vec3 planetCenter = { 0.0f, -PlanetRadiusMeters, 0.0f };
        float sdfPlanet = Anatomical::SDF::Sphere(p, planetCenter, PlanetRadiusMeters);
        float gyroid = Anatomical::SDF::Gyroid(p, CaveScale) - CaveWallThickness;
        return std::max(sdfPlanet, -gyroid);
    }
};

USTRUCT(BlueprintType)
struct FAnatomicalHitResult {
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Anatomical Engine")
    bool bHit = false;

    UPROPERTY(BlueprintReadOnly, Category = "Anatomical Engine")
    FVector ImpactPointUE = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Anatomical Engine")
    FVector ImpactNormalUE = FVector::UpVector;

    UPROPERTY(BlueprintReadOnly, Category = "Anatomical Engine")
    float DistanceMeters = 0.0f;
};

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class ANATOMICALENGINE_API UAnatomicalWorldComponent : public UActorComponent {
    GENERATED_BODY()

public:
    UAnatomicalWorldComponent() {
        PrimaryComponentTick.bCanEverTick = true;
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anatomical Engine|World")
    float SpatialCellSizeMeters = 500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anatomical Engine|Physics")
    bool bEnableHullCollision = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anatomical Engine|Physics")
    float OwnerHullRadiusMeters = 15.0f;

protected:
    virtual void BeginPlay() override {
        Super::BeginPlay();
        HashGrid = MakeUnique<Anatomical::SpatialHashGrid>(SpatialCellSizeMeters);
        WorldField = MakeUnique<FUE5AnalyticalPlanetWorld>();
    }

public:
    virtual void TickComponent(
        float DeltaTime, 
        ELevelTick TickType, 
        FActorComponentTickFunction* ThisTickFunction
    ) override {
        Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

        if (bEnableHullCollision && GetOwner() && HashGrid && WorldField) {
            FVector uePos = GetOwner()->GetActorLocation();
            Anatomical::Vec3 corePos = UEToCorePosition(uePos);
            Anatomical::Vec3 coreVel = UEToCoreVector(GetOwner()->GetVelocity());

            bool bCollided = Anatomical::FieldPhysicsSolver::ResolveSphereCollision(
                *WorldField,
                *HashGrid,
                corePos,
                coreVel,
                OwnerHullRadiusMeters,
                0.35f
            );

            if (bCollided) {
                GetOwner()->SetActorLocation(CoreToUEPosition(corePos));
                if (UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent())) {
                    if (RootPrim->IsSimulatingPhysics()) {
                        RootPrim->SetPhysicsLinearVelocity(CoreToUEVector(coreVel));
                    }
                }
            }
        }
    }

    // Спавн 256-байтного оператора кратера из Blueprints или C++
    UFUNCTION(BlueprintCallable, Category = "Anatomical Engine|Destruction")
    void SpawnCraterOperator(FVector WorldLocationUE, float RadiusMeters = 115.0f, float DepthMeters = 230.0f) {
        if (!HashGrid || !GetWorld()) return;

        Anatomical::PerturbationOperator op;
        op.position = UEToCorePosition(WorldLocationUE);
        op.radius = RadiusMeters;
        op.intensity = DepthMeters;
        op.timestamp = GetWorld()->GetTimeSeconds();
        op.temperatureK = 2800.0f;
        op.anisotropy = { 1.15f, 0.92f, 1.08f };

        HashGrid->InsertOperator(op);
    }

    // Серверная трассировка луча без генерации полигональных мешей (для баллистики и ИИ)
    UFUNCTION(BlueprintCallable, Category = "Anatomical Engine|Physics")
    FAnatomicalHitResult RaycastAnalyticalWorld(FVector StartUE, FVector DirectionUE, float MaxDistanceMeters = 25000.0f) {
        FAnatomicalHitResult OutResult;
        if (!HashGrid || !WorldField) return OutResult;

        Anatomical::Vec3 origin = UEToCorePosition(StartUE);
        Anatomical::Vec3 dir = UEToCoreVector(DirectionUE).Normalized();

        Anatomical::RayHitResult hit = Anatomical::FieldPhysicsSolver::RayCast(
            *WorldField,
            *HashGrid,
            origin,
            dir,
            MaxDistanceMeters,
            128
        );

        OutResult.bHit = hit.hit;
        OutResult.DistanceMeters = hit.distance;
        OutResult.ImpactPointUE = CoreToUEPosition(hit.point);
        OutResult.ImpactNormalUE = CoreToUEVector(hit.normal);
        return OutResult;
    }

    UFUNCTION(BlueprintPure, Category = "Anatomical Engine|Telemetry")
    int32 GetActiveOperatorCount() const {
        return HashGrid ? HashGrid->GetActiveCount() : 0;
    }

private:
    TUniquePtr<Anatomical::SpatialHashGrid> HashGrid;
    TUniquePtr<FUE5AnalyticalPlanetWorld> WorldField;

    // Конвертация систем координат: UE5 (см, Z-вверх) <-> AnatomicalCore (м, Y-вверх)
    static Anatomical::Vec3 UEToCorePosition(const FVector& uePos) {
        return {
            static_cast<float>(uePos.X * 0.01),
            static_cast<float>(uePos.Z * 0.01),
            static_cast<float>(uePos.Y * 0.01)
        };
    }

    static FVector CoreToUEPosition(const Anatomical::Vec3& corePos) {
        return FVector(
            static_cast<double>(corePos.x * 100.0f),
            static_cast<double>(corePos.z * 100.0f),
            static_cast<double>(corePos.y * 100.0f)
        );
    }

    static Anatomical::Vec3 UEToCoreVector(const FVector& ueVec) {
        return {
            static_cast<float>(ueVec.X * 0.01),
            static_cast<float>(ueVec.Z * 0.01),
            static_cast<float>(ueVec.Y * 0.01)
        };
    }

    static FVector CoreToUEVector(const Anatomical::Vec3& coreVec) {
        return FVector(
            static_cast<double>(coreVec.x * 100.0f),
            static_cast<double>(coreVec.z * 100.0f),
            static_cast<double>(coreVec.y * 100.0f)
        );
    }
};
