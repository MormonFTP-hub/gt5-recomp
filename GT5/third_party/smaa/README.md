# SMAA (third-party, MIT)

Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez's reference SMAA,
from https://github.com/iryoku/smaa (commit 71c806a838bdd7d517df19192a20f0c61b3ca29d), unmodified:

- `SMAA.hlsl`: the shader library
- `AreaTex.h`, `SearchTex.h`: its two precomputed lookup textures (from that repository's `Textures` folder)
- `LICENSE.txt`: its licence

The game's built-in anti-aliasing (`LD_AA=smaa`, the in-game menu's and the launcher's **SMAA**) runs it as
three compute passes over the 3D picture: `GT5/src/aa/gt5_smaa.cpp`.
