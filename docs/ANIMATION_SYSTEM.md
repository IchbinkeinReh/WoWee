# Animation System

Every unit - the player, other players, creatures and the mounts under them -
is animated by the same rules, the original client's. There is no state machine
of our own: a unit's movement flags, stand state, casts and swings say what it
plays, and `CharacterRenderer` plays it.

## How a unit's animation is chosen

`CharacterRenderer` keeps a unit's state (`upper_body::Unit`, handed over each
frame by `src/core/application.cpp`) and plays two layers of it: the whole
body, and the upper body where the legs go on with another animation.

1. **Asked** (`CharacterRenderer::playAnimation`, 0x007385c0): an emote, a
   swing, a spell visual kit's animation, a jump. Asked of one that rides, it
   is its mount's where a mount has it (`rider_mount::takes`), the rider's own
   body keeping its pose.
2. **Base** (`playBaseAnimation`, 0x0073ac30 -> 0x00724500): what the unit is
   in when nothing else is asked of it - dead, asleep, stunned, swimming,
   flying, running, its combat ready, its emote state, standing. Asked again at
   each event of its movement (`mover_event`, 0x0073ed10) and at the end of a
   one-shot (0x0073b510).
3. **The model's own answer** (`anim::AnimationDataTable`): a sequence the
   model lacks is replaced by the one AnimationData.dbc's Fallback leads to
   (0x00826350); a looping one is rolled again among its variations, which is
   where a standing unit's or mount's fidgets come from.

## File map (`include/rendering/animation/`, `src/rendering/animation/`)

| File | Purpose |
|---|---|
| `animation_ids` | `anim::` constants for every AnimationData.dbc id, names, the check against the DBC |
| `animation_data` | AnimationData.dbc: fallbacks, behaviours, weapon flags |
| `base_animation` | 0x00724500: the base animation, and what follows a one-shot |
| `upper_body` | The unit's state, and which animations go on the upper body alone |
| `mover_event` | What an event of the player's own movement does to its animation |
| `kit_animation` | What a spell visual kit does to the unit's animation (0x0073b140) |
| `rider_mount` | What the mount under a rider plays |
| `mount_motion` | The mount's bank in flight, the rider's bob, its idle sounds |
| `jump_landing` | JumpEnd, JumpLandRun or straight back to moving (0x0073d2b0) |
| `body_yaw` | The yaw a body is drawn at, apart from the facing it moves at |
| `melee_anim_chains` | Attack fallbacks per weapon kind |
| `weapon_type` | `WeaponLoadout`, the ranged weapon kinds |
| `emote_registry` | Emotes.dbc: emote name and id to animation |
| `footstep_driver` | The prints and splashes of the player's and its mount's steps |

`AnimationController` (`include/rendering/animation_controller.hpp`) is the
player's link between the camera controller and the renderer: it attaches the
mount, seats the rider and plays what the interface asks of the player.

Other units are driven from `src/core/`: the per-frame loops in
`application.cpp`, `entity_spawner_processing.cpp`, and the combat, emote,
jump, mount special and death callbacks in `animation_callback_handler.cpp`.

## Sounds of an animation

A model's animation carries events (`$FSD`, `$CSS`, `$CAH`, `$FD1`, ...), which
`EntitySpawner::playUnitModelEvents` answers each frame: the footsteps, swings,
impacts, fidgets and the unit's CreatureSoundData slots
(`rendering/unit_model_events.hpp`, `rendering/melee_sound.hpp`).

## Adding to it

Find what the client does first. Each rule above names the function it is a
port of; a new one goes beside it as a pure function of the unit's state with a
test, and the renderer asks it.

## Tests

`test_base_animation`, `test_upper_body`, `test_mover_event`,
`test_kit_animation`, `test_rider_mount`, `test_jump_landing`,
`test_animation_data`, `test_animation_ids`, `test_melee_anim_chains`,
`test_body_yaw`, `test_mount_special`, `test_footstep_paths`,
`test_m2_footstep_events`.
