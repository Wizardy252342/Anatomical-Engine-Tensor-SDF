import math
import os
import struct
import sys
import time


class ContinentOBJExporter:
    """
    Конвертер спектральных миров (.acont) в стандартный 3D-меш (.obj).
    Позволяет открывать сжатые ландшафты Anatomical Engine в Blender,
    Unreal Engine 5, Unity и стандартных 3D-просмотрщиках.
    """

    def __init__(self, atlas_path="compiled_continent.acont"):
        self.atlas_path = atlas_path
        self.grid_x = 1
        self.grid_z = 1
        self.harms_per_chunk = 24
        self.chunk_size = 3000.0
        self.rock_detail = 16.0
        self.chunks = []

    def load_atlas(self):
        if not os.path.exists(self.atlas_path):
            raise FileNotFoundError(f"Атлас не найден: {self.atlas_path}")

        with open(self.atlas_path, "rb") as f:
            header = f.read(64)
            magic, gx, gz, h_count, c_size, r_det = struct.unpack("<4sIIIff", header[:24])
            if magic != b"ACNT":
                raise ValueError("Неверный формат файла .acont!")

            self.grid_x = gx
            self.grid_z = gz
            self.harms_per_chunk = h_count
            self.chunk_size = c_size
            self.rock_detail = r_det

            total_chunks = gx * gz
            self.chunks = []
            for _ in range(total_chunks):
                chunk_harms = []
                for _ in range(h_count):
                    amp, fx, fz, phase = struct.unpack("<4f", f.read(16))
                    chunk_harms.append((amp, fx, fz, phase))
                self.chunks.append(chunk_harms)

        print(f"[OBJ Exporter] Loaded {self.atlas_path}: {self.grid_x}x{self.grid_z} chunks")

    def evaluate_escher_rock(self, x, y, z):
        """Повторяет ортогональный шум SO(3) из C++ ядра для детализации скал."""
        freq = 0.015
        qx, qy, qz = x * freq, y * freq, z * freq
        n1 = math.sin(qx) * math.cos(qy) * math.sin(qz)

        rx = 0.36 * qx + 0.48 * qy - 0.80 * qz
        ry = -0.80 * qx + 0.60 * qy
        rz = 0.48 * qx + 0.64 * qy + 0.60 * qz

        qx2, qy2, qz2 = rx * 2.13, ry * 2.13, rz * 2.13
        n2 = math.sin(qx2) * math.cos(qy2) * math.sin(qz2) * 0.45
        return (n1 + n2) * (self.rock_detail * 0.5)

    def evaluate_height_at(self, wx, wz):
        half_total_x = (self.grid_x * self.chunk_size) * 0.5
        half_total_z = (self.grid_z * self.chunk_size) * 0.5

        cx = int(math.floor((wx + half_total_x) / self.chunk_size))
        cz = int(math.floor((wz + half_total_z) / self.chunk_size))
        cx = max(0, min(self.grid_x - 1, cx))
        cz = max(0, min(self.grid_z - 1, cz))

        chunk_center_x = (cx + 0.5) * self.chunk_size - half_total_x
        chunk_center_z = (cz + 0.5) * self.chunk_size - half_total_z
        lx = wx - chunk_center_x
        lz = wz - chunk_center_z

        harms = self.chunks[cz * self.grid_x + cx]
        h = 0.0
        for amp, fx, fz, phase in harms:
            h += amp * math.cos(lx * fx + lz * fz + phase)

        h += self.evaluate_escher_rock(wx, h, wz)
        return h

    def export_obj(self, out_obj="continent_export.obj", resolution=180, scale_export=0.01):
        """
        Генерирует сетку (resolution x resolution), вычисляет нормали
        и сохраняет 3D-модель в Wavefront .OBJ (масштаб 0.01 = 100м в 1 единице Blender).
        """
        t0 = time.perf_counter()
        total_w = self.grid_x * self.chunk_size
        total_d = self.grid_z * self.chunk_size
        half_w = total_w * 0.5
        half_d = total_d * 0.5

        print(f"[OBJ Exporter] Generating {resolution}x{resolution} mesh ({resolution * resolution} vertices)...")

        heights = [[0.0] * resolution for _ in range(resolution)]
        for iz in range(resolution):
            wz = -half_d + (iz / float(resolution - 1)) * total_d
            for ix in range(resolution):
                wx = -half_w + (ix / float(resolution - 1)) * total_w
                heights[iz][ix] = self.evaluate_height_at(wx, wz)

        step_x = total_w / float(resolution - 1)
        step_z = total_d / float(resolution - 1)

        with open(out_obj, "w", encoding="utf-8") as f:
            f.write("# Exported by Anatomical Engine OBJ Mesher v1.0\n")
            f.write(f"# Source: {self.atlas_path} ({total_w / 1000.0:.1f}x{total_d / 1000.0:.1f} km)\n")
            f.write("o AnatomicalContinent\n")

            # 1. Записываем вершины (v) и UV-координаты (vt)
            for iz in range(resolution):
                wz = -half_d + iz * step_z
                v_coord = iz / float(resolution - 1)
                for ix in range(resolution):
                    wx = -half_w + ix * step_x
                    wy = heights[iz][ix]
                    u_coord = ix / float(resolution - 1)
                    f.write(f"v {wx * scale_export:.4f} {wy * scale_export:.4f} {wz * scale_export:.4f}\n")
                    f.write(f"vt {u_coord:.4f} {v_coord:.4f}\n")

            # 2. Вычисляем и записываем плавные нормали вершин (vn)
            for iz in range(resolution):
                for ix in range(resolution):
                    h_l = heights[iz][max(0, ix - 1)]
                    h_r = heights[iz][min(resolution - 1, ix + 1)]
                    h_d = heights[max(0, iz - 1)][ix]
                    h_u = heights[min(resolution - 1, iz + 1)][ix]

                    nx = (h_l - h_r) / (2.0 * step_x)
                    ny = 1.0
                    nz = (h_d - h_u) / (2.0 * step_z)
                    inv_len = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
                    f.write(f"vn {nx * inv_len:.4f} {ny * inv_len:.4f} {nz * inv_len:.4f}\n")

            # 3. Записываем треугольники (f v/vt/vn)
            for iz in range(resolution - 1):
                for ix in range(resolution - 1):
                    v0 = iz * resolution + ix + 1
                    v1 = v0 + 1
                    v2 = (iz + 1) * resolution + ix + 1
                    v3 = v2 + 1

                    f.write(f"f {v0}/{v0}/{v0} {v2}/{v2}/{v2} {v1}/{v1}/{v1}\n")
                    f.write(f"f {v1}/{v1}/{v1} {v2}/{v2}/{v2} {v3}/{v3}/{v3}\n")

        elapsed = time.perf_counter() - t0
        size_mb = os.path.getsize(out_obj) / (1024.0 * 1024.0)
        tris = (resolution - 1) * (resolution - 1) * 2
        print(f"[OBJ Exporter] Success in {elapsed:.2f}s! File: {out_obj} ({size_mb:.2f} MB, {tris} triangles)")


if __name__ == "__main__":
    atlas = sys.argv[1] if len(sys.argv) > 1 else "compiled_continent.acont"
    out_file = sys.argv[2] if len(sys.argv) > 2 else "continent_export.obj"
    exporter = ContinentOBJExporter(atlas)
    exporter.load_atlas()
    exporter.export_obj(out_file, resolution=180)
