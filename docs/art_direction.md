# Visual direction

The target is broadly in the spirit of **Risk of Rain 2**, without copying its
assets or treating its few full-planet views as a template.

- Favor economical meshes with smooth shading and strong silhouettes. Low-poly
  does not mean every triangle must have a hard normal or a different color.
- Use intentional large color regions, readable value contrast and cool/warm
  separation. Avoid covering surfaces in uniform tiny noise.
- Effects should have designed shapes and restrained glow. Bloom supports a
  bright feature; it should not wash out the rest of the image.
- Preserve enough structure to distinguish the subject: ships need purposeful
  forms, Earth needs recognizable continents. Simplification is not randomization.
- Render the same content through the same shader logic in the editor and game.
  Inspect actual screenshots alongside geometry/ID diagnostics.

This is a direction for future work, not a claim that existing sun, ship and rock
assets have already been restyled. [Earth](earth.md) is the first explicit trial:
smooth geometry, simplified geographic shapes and opt-in illustrated lighting.
