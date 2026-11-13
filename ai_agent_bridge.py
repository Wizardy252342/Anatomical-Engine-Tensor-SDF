import ctypes
import os

dll_path = os.path.abspath("AnatomicalCore.dll")
core = ctypes.CDLL(dll_path)

# Настройка типов аргументов и возвращаемых значений C-API
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

# 1. Инициализация ядра для центрального ИИ-агента
ctx = core.Anatomical_CreateContext(500.0)
print(f"Initial Merkle Root Hash: {hex(core.Anatomical_GetGlobalRootHash(ctx))}")

# 2. Запрос расстояния до ближайшей стены пещеры (0 GPU overhead)
sdf_val = core.Anatomical_EvaluateSDF(ctx, 0.0, 0.0, 1000.0)
print(f"AI Agent Distance to Nearest Wall: {sdf_val:.2f} meters")

# 3. ИИ-агент применяет оператор разрушения и получает обновленный хеш дерева
new_hash = core.Anatomical_InsertOperator(ctx, 120.0, -50.0, 1000.0, 115.0, 230.0, 1.0)
print(f"Post-Perturbation Merkle Root Hash: {hex(new_hash)}")

core.Anatomical_DestroyContext(ctx)
