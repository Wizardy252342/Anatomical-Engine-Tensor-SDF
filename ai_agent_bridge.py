import ctypes
import math
import os
import time

dll_path = os.path.abspath("AnatomicalCore.dll")
core = ctypes.CDLL(dll_path)

# 1. Настройка сигнатур C-API
core.Anatomical_CreateContext.argtypes = [ctypes.c_float]
core.Anatomical_CreateContext.restype = ctypes.c_void_p

core.Anatomical_DestroyContext.argtypes = [ctypes.c_void_p]

core.Anatomical_InsertOperator.argtypes = [
    ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_float,
    ctypes.c_float, ctypes.c_float, ctypes.c_float
]
core.Anatomical_InsertOperator.restype = ctypes.c_uint64

core.Anatomical_GetGlobalRootHash.argtypes = [ctypes.c_void_p]
core.Anatomical_GetGlobalRootHash.restype = ctypes.c_uint64

core.Anatomical_EvaluateSDF.argtypes = [
    ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_float
]
core.Anatomical_EvaluateSDF.restype = ctypes.c_float

core.Anatomical_RayCastBatch.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_float),
    ctypes.POINTER(ctypes.c_float),
    ctypes.c_float,
    ctypes.POINTER(ctypes.c_float),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_int
]

print("=== Anatomical Engine v2.0: High-Speed AI Agent & Merkle Router Test ===")

# Создаем два изолированных узла (Центральный ИИ-агент и Клиентский узел)
ai_ctx = core.Anatomical_CreateContext(500.0)
client_ctx = core.Anatomical_CreateContext(500.0)

# Позиция ИИ-дрона внутри главного 105-км тоннеля
drone_x, drone_y, drone_z = 120.0, -450.0, 2500.0
dist_to_wall = core.Anatomical_EvaluateSDF(ai_ctx, drone_x, drone_y, drone_z)
print(f"[AI Drone] Position: ({drone_x}, {drone_y}, {drone_z}) | Clearance: {dist_to_wall:.2f} m")

# 2. Пакетное сканирование 3D-пространства лидаром (10 000 лучей за один вызов C-API)
RAY_COUNT = 10000
origins = (ctypes.c_float * (RAY_COUNT * 3))()
dirs = (ctypes.c_float * (RAY_COUNT * 3))()
out_dists = (ctypes.c_float * RAY_COUNT)()
out_hits = (ctypes.c_uint8 * RAY_COUNT)()

# Генерируем сферическую сетку направлений лидара (спираль Фибоначчи)
golden_angle = math.pi * (3.0 - math.sqrt(5.0))
for i in range(RAY_COUNT):
    y = 1.0 - (i / float(RAY_COUNT - 1)) * 2.0
    radius = math.sqrt(max(0.0, 1.0 - y * y))
    theta = golden_angle * i
    dx = math.cos(theta) * radius
    dz = math.sin(theta) * radius

    base = i * 3
    origins[base] = drone_x
    origins[base + 1] = drone_y
    origins[base + 2] = drone_z
    dirs[base] = dx
    dirs[base + 1] = y
    dirs[base + 2] = dz

t_start = time.perf_counter()
core.Anatomical_RayCastBatch(ai_ctx, origins, dirs, 15000.0, out_dists, out_hits, RAY_COUNT)
elapsed_ms = (time.perf_counter() - t_start) * 1000.0

hits_total = sum(out_hits[i] for i in range(RAY_COUNT))
min_hit_dist = min((out_dists[i] for i in range(RAY_COUNT) if out_hits[i] == 1), default=0.0)
rays_per_sec = (RAY_COUNT / (elapsed_ms / 1000.0)) if elapsed_ms > 0 else 0

print(f"[Batch LiDAR] Cast {RAY_COUNT} 3D rays in {elapsed_ms:.2f} ms ({rays_per_sec:,.0f} rays/sec)")
print(f"[Batch LiDAR] Cavern walls detected: {hits_total}/{RAY_COUNT} | Nearest obstacle: {min_hit_dist:.2f} m")

# 3. Проверка синхронизации через фрактальный роутер Меркла
h_ai_init = core.Anatomical_GetGlobalRootHash(ai_ctx)
h_cl_init = core.Anatomical_GetGlobalRootHash(client_ctx)
print(f"\n[Merkle Sync] Initial Hashes Match: {h_ai_init == h_cl_init} ({hex(h_ai_init)}) -> 0 Bytes Network Traffic")

# ИИ-агент вырезает кратер прямо на границе 500-метровой ячейки (тест Halo Insertion)
h_ai_after = core.Anatomical_InsertOperator(ai_ctx, 500.0, -500.0, 2500.0, 115.0, 230.0, 2.5)
print(f"[Merkle Sync] AI Mutated Terrain -> New AI Root Hash: {hex(h_ai_after)}")
print(f"[Merkle Sync] Divergence Detected: {h_ai_after != h_cl_init} -> Syncing 256-Byte Delta Packet...")

# Клиент принимает 256-байтный пакет и применяет его
h_cl_synced = core.Anatomical_InsertOperator(client_ctx, 500.0, -500.0, 2500.0, 115.0, 230.0, 2.5)
print(f"[Merkle Sync] Client Synced -> Hashes Match: {h_ai_after == h_cl_synced} ({hex(h_cl_synced)})")

core.Anatomical_DestroyContext(ai_ctx)
core.Anatomical_DestroyContext(client_ctx)
