# MatPacker

Simple application to convert normal image formats into KTX2 and pack components together.

## Output

|                   | R                 | G         | B        | A                |
| ----------------- | ----------------- | --------- | -------- | ---------------- |
| Image 1 (diffuse) | Albedo R          | Albedo G  | Albedo B | Albedo A         |
| Image 2 (normal)  | Normal R          | Normal G  | Normal B | Unused (for now) |
| Image 3 (ORM)     | Ambient Occlusion | Roughness | Metallic | Unused (for now) |
