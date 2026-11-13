import json
import math
import os
import struct
import sys
import time


class ContinentWorldCompiler:
    """
    Компилятор континентов Anatomical Engine (v2.0).
    Нарезает карту высот на сетку чанков (grid_x x grid_z), сжимает каждый сектор
    в 24 главные спектральные гармоники (384 байта/чанк) и упаковывает в атлас .acont.
    """

    MAGIC = b"ACNT"
    HARMONICS_PER_CHUNK = 24

    def __init__(self, recipe_path="world_recipe.json"):
        self.recipe_path = recipe_path
        self.recipe = {}
        self.full_w = 512
        self.full_h = 512
        self.full_heightmap = []

    def load_recipe(self):
        if not os.path.exists(self.recipe_path):
            raise FileNotFoundError(f"Рецепт не найден: {self.recipe_path}")
        with open(self.recipe_path, "r", encoding="utf-8") as f:
            self.recipe = json.load(f)
        print(f"[ContinentCompiler] Recipe: {self.recipe.get('world_name', 'Continent')}")

    def build_master_heightfield(self):
        grid_x = int(self.recipe.get("grid_chunks_x", 4))
        grid_z = int(self.recipe.get("grid_chunks_z", 4))
        # Разрешение 64x64 пикселя на каждый чанк для высокой детализации нарезки
        self.full_w = grid_x * 64
        self.full_h = grid_z * 64

        img_path = self.recipe.get("source_image", "")
        max_h = float(self.recipe.get("max_height_meters", 600.0))
        invert = bool(self.recipe.get("invert_heights", False))

        if img_path and os.path.exists(img_path):
            try:
                from PIL import Image
                img = Image.open(img_path).convert("L").resize((self.full_w, self.full_h))
                pixels = list(img.getdata())
                self.full_heightmap = []
                for y in range(self.full_h):
                    row = []
                    for x in range(self.full_w):
                        norm = pixels[y * self.full_w + x] / 255.0
                        if invert:
                            norm = 1.0 - norm
                        # Добавляем континентальные горные цепи по краям
                        nx = (x / float(self.full_w - 1)) * 2.0 - 1.0
                        nz = (y / float(self.full_h - 1)) * 2.0 - 1.0
                        ridge = 0.25 * max_h * math.sin(nx * 6.0) * math.cos(nz * 6.0)
                        row.append(norm * max_h + ridge)
                    self.full_heightmap.append(row)
                print(f"[ContinentCompiler] Master heightmap built from {img_path}: {self.full_w}x{self.full_h}")
                return
            except ImportError:
                print("[Warning] Pillow not installed. Using procedural continent.")

        self.full_heightmap = [[0.0] * self.full_w for _ in range(self.full_h)]
        for y in range(self.full_h):
            nz = (y / float(self.full_h - 1)) * 2.0 - 1.0
            for x in range(self.full_w):
                nx = (x / float(self.full_w - 1)) * 2.0 - 1.0
                r = math.sqrt(nx * nx + nz * nz)
                crater = -0.45 * max_h * math.exp(-0.5 * (r / 0.35) ** 2)
                mountains = 0.35 * max_h * math.sin(nx * 8.0) * math.cos(nz * 7.0)
                canyon = -0.3 * max_h * math.exp(-0.5 * (abs(nx + 0.3 * math.sin(nz * 5.0)) / 0.08) ** 2)
                self.full_heightmap[y][x] = crater + mountains + canyon + max_h * 0.4
        print(f"[ContinentCompiler] Procedural continent generated: {self.full_w}x{self.full_h}")

    def compress_single_chunk(self, subgrid, chunk_size):
        h = len(subgrid)
        w = len(subgrid[0])
        half_size = chunk_size * 0.5
        inv_samples = 1.0 / float(w * h)
        candidates = []
        max_freq = 10

        for kz in range(-max_freq, max_freq + 1):
            for kx in range(0, max_freq + 1):
                if kx == 0 and kz < 0:
                    continue
                fx = (kx * math.pi) / half_size
                fz = (kz * math.pi) / half_size
                sum_cos = 0.0
                sum_sin = 0.0

                for iy in range(h):
                    wz = ((iy / float(h - 1)) * 2.0 - 1.0) * half_size
                    row = subgrid[iy]
                    for ix in range(w):
                        wx = ((ix / float(w - 1)) * 2.0 - 1.0) * half_size
                        val = row[ix]
                        angle = wx * fx + wz * fz
                        sum_cos += val * math.cos(angle)
                        sum_sin += val * math.sin(angle)

                scale = 1.0 if (kx == 0 and kz == 0) else 2.0
                a_cos = (sum_cos * inv_samples) * scale
                a_sin = (sum_sin * inv_samples) * scale
                amp = math.sqrt(a_cos * a_cos + a_sin * a_sin)
                phase = -math.atan2(a_sin, a_cos)
                candidates.append((amp, fx, fz, phase))

        candidates.sort(key=lambda item: item, reverse=True)
        top = candidates[: self.HARMONICS_PER_CHUNK]
        while len(top) < self.HARMONICS_PER_CHUNK:
            top.append((0.0, 0.0, 0.0, 0.0))
        return top

    def compile_and_save(self, out_path="compiled_continent.acont"):
        t0 = time.perf_counter()
        grid_x = int(self.recipe.get("grid_chunks_x", 4))
        grid_z = int(self.recipe.get("grid_chunks_z", 4))
        chunk_size = float(self.recipe.get("chunk_size_meters", 3000.0))
        rock_detail = float(self.recipe.get("rock_detail_strength", 16.0))
        c_low = self.recipe.get("contour_color_low", [0.1, 0.75, 1.0])
        c_high = self.recipe.get("contour_color_high", [1.0, 0.65, 0.2])

        # Заголовок 64 байта
        header = struct.pack(
            "<4sIIIff3f3f4I",
            self.MAGIC,
            grid_x,
            grid_z,
            self.HARMONICS_PER_CHUNK,
            chunk_size,
            rock_detail,
            *map(float, c_low),
            *map(float, c_high),

            0, 0, 0, 0
        )

        with open(out_path, "wb") as f:
            f.write(header)
            total_chunks = grid_x * grid_z
            idx = 0
            for cz in range(grid_z):
                for cx in range(grid_x):
                    idx += 1
                    # Вырезаем окно с небольшим перекрытием (+4 пикселя) для плавного стыка
                    x0 = max(0, cx * 64 - 2)
                    x1 = min(self.full_w, (cx + 1) * 64 + 2)
                    z0 = max(0, cz * 64 - 2)
                    z1 = min(self.full_h, (cz + 1) * 64 + 2)
                    subgrid = [row[x0:x1] for row in self.full_heightmap[z0:z1]]

                    harms = self.compress_single_chunk(subgrid, chunk_size)
                    for amp, fx, fz, phase in harms:
                        f.write(struct.pack("<4f", amp, fx, fz, phase))
                    print(f"  -> Compressed chunk [{cx}, {cz}] ({idx}/{total_chunks})")

        elapsed = (time.perf_counter() - t0)
        total_km = (grid_x * chunk_size) / 1000.0
        size_kb = os.path.getsize(out_path) / 1024.0
        print(f"[ContinentCompiler] Done in {elapsed:.2f}s! Continent: {total_km:.1f}x{total_km:.1f} km ({size_kb:.2f} KB)")


if __name__ == "__main__":
    recipe_file = sys.argv[1] if len(sys.argv) > 1 else "world_recipe.json"
    comp = ContinentWorldCompiler(recipe_file)
    comp.load_recipe()
    comp.build_master_heightfield()
    comp.compile_and_save("compiled_continent.acont")
