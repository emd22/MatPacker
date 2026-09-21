# MatPacker

Simple tool to convert image formats into KTX2 and pack multiple textures together.

## Features

- Select a few images, and they are auto tagged based on their filenames (e.g. `some_material_diffuse.jpg`)
- Load GLTF models, and select each material to pack
- Scale down materials to 1/2, 1/4, or 1/8th size
- Export all images as compressed KTX2 images, ready to be used in game engines
- Preview mips for each baked texture after baking

## Output

|                   | R                 | G         | B        | A                |
| ----------------- | ----------------- | --------- | -------- | ---------------- |
| Image 1 (diffuse) | Albedo R          | Albedo G  | Albedo B | Albedo A         |
| Image 2 (normal)  | Normal R          | Normal G  | Normal B | Unused (for now) |
| Image 3 (ORM)     | Ambient Occlusion | Roughness | Metallic | Unused (for now) |
