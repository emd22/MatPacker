# MatPacker

A tool designed to convert images into KTX2 textures and pack them into game ready textures. You can also pull materials from GLTF models!

<img src="Screenshots/matpacker2.png" width="400" height="600" />

## Features

- Select a few images, and they are auto tagged based on their filenames (e.g. `some_material_diffuse.jpg`)
- Load GLTF models, and select each material to pack
- Scale down materials to 1/2, 1/4, or 1/8th size
- Export all images as compressed KTX2 images, ready to be used in game engines
- Preview mips for each baked texture after baking
- Export a copy of a GLTF model with its materials linked to the baked KTX2 textures (via `KHR_texture_basisu`), optionally embedding the textures in the file

## Output

|                   | R                 | G         | B        | A                |
| ----------------- | ----------------- | --------- | -------- | ---------------- |
| Image 1 (diffuse) | Albedo R          | Albedo G  | Albedo B | Albedo A         |
| Image 2 (normal)  | Normal R          | Normal G  | Normal B | Unused (for now) |
| Image 3 (ORM)     | Ambient Occlusion | Roughness | Metallic | Unused (for now) |

## AI Disclaimer

LLMs were used in the original prototype of this application, and are used to find and fix bugs.
