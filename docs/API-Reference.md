# API Reference

Full reference for `include/FakeBots.inc`. See [Getting-Started.md](Getting-Started.md)
for usage examples.

## Constants

| Constant | Value | Meaning |
|---|---|---|
| `FAKEBOTS_INVALID_ID` | -1 | Returned by natives on failure / used as an "unset" id |
| `FAKEBOTS_MAX_BOTS` | 1000 | Hard ceiling on concurrent bots |
| `FAKEBOTS_MOVE_WALK` / `_RUN` / `_SPRINT` | 1 / 2 / 3 | On-foot movement style |
| `FAKEBOTS_STATE_CONNECTING` | 0 | RakNet client connection in progress |
| `FAKEBOTS_STATE_IDLE` | 1 | Connected, not spawned |
| `FAKEBOTS_STATE_SPAWNED` | 2 | Alive in the world |
| `FAKEBOTS_STATE_DEAD` | 3 | Wasted |
| `FAKEBOTS_STATE_REMOVING` | 4 | Being destroyed |

## Creation / Destruction

### `FakeBotCreate(const name[], skinid, Float:x, Float:y, Float:z, Float:angle = 0.0, weapon = 0, ammo = 0, virtualworld = 0, interior = 0)`
Begins creating a bot. Returns a **bot id** immediately, but the bot is not
usable until `OnFakeBotConnect` fires for it (the embedded RakNet client connection
handshake is asynchronous; connections are paced, so 100 bots take a few seconds).

Returns `FAKEBOTS_INVALID_ID` (-1), and writes the reason to the server log, when

* the nickname is empty, shorter than 3 or longer than 24 characters, or contains a blank, a control
  or a non-ASCII character,
* the nickname is already used by a player or another bot,
* no player slot is left: bots never take the last `FakeBotGetReservedSlots()` slots (default 1),
* `FAKEBOTS_MAX_BOTS` bots exist, or the server port cannot be determined.

The server applies its own nickname rules as well (characters, a customised
`name_characters`); if it refuses the nickname the script is told through
`OnFakeBotDisconnect(botid, -2002)`.

### `FakeBotDestroy(botid)`
Disconnects and frees a bot. Returns `true` on success.

### `FakeBotRespawn(botid, skinid = -1, Float:x = FAKEBOTS_RESPAWN_KEEP, Float:y = FAKEBOTS_RESPAWN_KEEP, Float:z = FAKEBOTS_RESPAWN_KEEP, Float:angle = FAKEBOTS_RESPAWN_KEEP)`
Respawns a bot. `skinid = -1` and `FAKEBOTS_RESPAWN_KEEP` (a NaN sentinel, so that
`0.0` is a usable coordinate) keep the current value. The bot walks the normal class
selection / spawn handshake again.

### `FakeBotIsValid(botid)`
Returns `true` if the bot id refers to a live slot.

### `FakeBotGetCount()`
Number of bots currently connected or connecting.

### `FakeBotGetState(botid)`
Returns one of the `FAKEBOTS_STATE_*` constants.

### `FakeBotGetName(botid, name[], size = sizeof(name))`
Copies the bot's name; returns the string length.

## Id mapping

### `FakeBotGetPlayerID(botid)`
Returns the real player id backing a bot. **This is your bridge to every
standard native** - `SetPlayerSkin`, `SetPlayerHealth`,
`GivePlayerWeapon`, `PutPlayerInVehicle`, `TogglePlayerControllable`, and
so on all work directly on this id.

### `FakeBotGetID(playerid)`
Reverse lookup. Returns `FAKEBOTS_INVALID_ID` if `playerid` isn't a bot.

### `FakeBotIsPlayer(playerid)`
Shorthand for `FakeBotGetID(playerid) != FAKEBOTS_INVALID_ID`.

### Automatic animation sync for bots

When `<a_samp>` is included before `<FakeBots>`, calls to the standard
Pawn `ApplyAnimation()` and `ClearAnimations()` made after the FakeBots
include are routed through stock wrappers.

Classic SA-MP 0.3.7 does not provide open.mp's persistent SYNC_OTHER
(`forcesync = 2`) behavior. For FakeBots, the wrapper therefore stores the
animation parameters in the plugin and calls `ApplyAnimation()` with
`forcesync = 1` so currently streamed-in players see it. The plugin then
re-applies the stored animation from its `OnPlayerStreamIn` hook whenever
the bot becomes visible to a player again. `ClearAnimations()` removes the
stored state and uses `forcesync = 1` so current viewers also receive the
clear operation. For real players, both wrappers preserve the caller's
original `forcesync` value.

This is a Pawn include-level redirection, not a runtime replacement of the
server natives. If a script must keep the original native names untouched,
define `FAKEBOTS_NO_AUTO_ANIMATION_SYNC` before including `<FakeBots>`.
The redirection only affects calls compiled after the FakeBots include.
The persistence mechanism applies to animations requested through the
FakeBots wrapper; direct server-native calls bypass the stored-state mechanism.

## Position / velocity physics

### `FakeBotSetPosition(playerid, Float:x, Float:y, Float:z)`

Teleports a spawned FakeBot to the requested position and starts server-side
gravity simulation. The bot keeps falling while gravity is active.

**ColAndreas is optional.** If the ColAndreas bridge is available, FakeBots
uses a downward raycast to detect a collision surface and stops the bot at the
hit position. To enable this, include `<colandreas>` before `<FakeBots>`,
load the ColAndreas plugin on the server, and call `CA_Init()` during
initialization.

Without ColAndreas, `FakeBotSetPosition()` still succeeds and gravity is still
simulated, but FakeBots cannot detect the ground or stop the fall automatically.
The bot will continue falling until another script action changes its position
or velocity. No landing height is supplied by the gamemode.

The headless client does not run GTA's own physics. ColAndreas collision
limitations apply when it is enabled: its static map collision does not model
every interior, virtual-world, per-player, or streamer object collision.
Dynamic objects must be registered with ColAndreas' collision system to be
detected. This is ground/surface landing detection, not full ped-vs-wall
collision.

### `FakeBotSetVelocity(playerid, Float:x, Float:y, Float:z)`

Sets velocity in GTA world units per second. The plugin integrates velocity
and gravity on each simulation tick while the bot is airborne.

Example: move a bot 8 units upward and let it fall onto the first surface below:

```pawn
new Float:x, Float:y, Float:z;
GetPlayerPos(playerid, x, y, z);
FakeBotSetPosition(playerid, x, y, z + 8.0);
```

To add an initial upward impulse, call `FakeBotSetVelocity()` after
`FakeBotSetPosition()`, for example `FakeBotSetVelocity(playerid, 0.0, 0.0, 2.0)`.

### `FakeBotSimulateKnockback(playerid, Float:velocityX, Float:velocityY, Float:upVelocity)`

Runs a self-contained knockback animation sequence for a spawned bot that is
not in a vehicle. The native captures the bot's current position, simulates a
ballistic rise and fall, then snaps it back to that exact position before
playing the landing and get-up animations.

`velocityX` and `velocityY` define the horizontal launch vector.
Horizontal displacement is deliberately capped at 0.75 world units and is
steered back to the captured position, preventing the effect from drifting
far sideways. `upVelocity` is the upward speed in GTA world units per second
and must be between 1.0 and 20.0. Only one knockback may run on a bot at a time.

The sequence uses the `PED` animations `FALL_fall`, `FALL_land`, and
`getup`. It does not perform full collision checks against ceilings,
walls, interiors, or streamed objects; the bounded horizontal vector reduces
sideways clipping but is not a substitute for collision geometry.

Example:

```pawn
// Mostly vertical knockback, with a small sideways vector.
FakeBotSimulateKnockback(playerid, 0.15, 0.0, 7.0);
```

## On-foot movement

### `FakeBotGoTo(botid, Float:x, Float:y, Float:z, Float:speed = 1.0, movetype = FAKEBOTS_MOVE_WALK)`
Walks/runs/sprints toward a single point in a straight line, animating and
facing the bot automatically. Fires `OnFakeBotReachDestination` on arrival.
`speed` is in GTA units/second (≈1.0 for a natural walking pace).

### `FakeBotStopMoving(botid)`
Cancels the current route.

### `FakeBotIsMoving(botid)`

### `FakeBotGetMoveTarget(botid, &Float:x, &Float:y, &Float:z)`

### `FakeBotAddWaypoint(botid, Float:x, Float:y, Float:z, Float:speed = 1.0, movetype = FAKEBOTS_MOVE_WALK)`
Appends a point to the bot's route; movement starts automatically. Call
repeatedly to build a multi-point patrol.

### `FakeBotClearWaypoints(botid)`

### `FakeBotSetLoopWaypoints(botid, bool:loop)`
When `true`, each waypoint is re-queued as it's reached, turning the route
into an endless loop.

## Vehicles / driving

### `FakeBotPutInNewVehicle(botid, vehiclemodel, Float:x, Float:y, Float:z, Float:angle, color1 = -1, color2 = -1)`
Creates a vehicle and seats the bot as the driver. Returns the vehicle id.

### `FakeBotPutInVehicle(botid, vehicleid, seatid = 0)`
Seats the bot in an existing vehicle (`0` = driver).

### `FakeBotRemoveFromVehicle(botid)`

### `FakeBotDriveTo(botid, Float:x, Float:y, Float:z, Float:speed = 30.0)`
Drives the bot's current vehicle to a point in a straight line. The bot
must already be the vehicle's driver. Fires
`OnFakeBotVehicleReachDest` on arrival. `speed` is in GTA
units/second (≈30-45 for city traffic pace).

> **Note on driving:** FakeBots drives vehicles by interpolating position
> and velocity directly (server-authoritative), not by simulating GTA's
> vehicle physics/AI or road-following. For routes that must stay on
> roads, feed `FakeBotAddWaypoint`-style dense point sequences (e.g.
> exported from your map's road nodes) into repeated `FakeBotDriveTo`
> calls chained from `OnFakeBotVehicleReachDest`.

### `FakeBotStopDriving(botid)`

### `FakeBotIsDriving(botid)`

### `FakeBotAddDriveWaypoint(botid, Float:x, Float:y, Float:z, Float:speed = 30.0)`
Appends a point to the bot's driving route (bot must already be a
driver). Movement starts automatically once at least one waypoint is
queued. Feed this a dense, road-following point sequence for routes that
need to stay on roads - `FakeBotDriveTo()` alone only goes in a straight
line.

### `FakeBotClearDriveWaypoints(botid)`

### `FakeBotSetLoopDriveWaypoints(botid, bool:loop)`
Turns the driving route into an endless loop (taxi/bus-style patrol).

## Follow AI

### `FakeBotFollow(botid, targetplayerid, Float:offsetx, Float:offsety, Float:speed = 2.2)`
Continuously steers the bot to stay near `targetplayerid` at a fixed
local offset that rotates with the target's facing angle. Re-evaluated
every tick, unlike the one-shot `FakeBotGoTo()`.

### `FakeBotStopFollowing(botid)` / `FakeBotIsFollowing(botid)`

## Combat AI (lightweight, opt-in)

### `FakeBotSetCombatTarget(botid, targetplayerid, Float:damageperhit = 5.0, Float:fireinterval = 1.0, Float:range = 20.0)`
While both bot and target are connected and alive, the bot walks into
range if needed, faces the target, plays a firing animation and reduces
the target's health by `damageperhit` every `fireinterval` seconds,
firing `OnFakeBotGiveDamage` each hit.

> **What this is and isn't:** a simple scripted combat model for
> ambient gunfights, not a hit-detection/ballistics simulation. There's
> no line-of-sight check, no cover-seeking, and it directly adjusts
> health rather than simulating bullets - weaponid/bodypart reported to
> `OnFakeBotGiveDamage` are always `0`. For anything more precise, track
> your own weapon state and call `SetPlayerHealth`/`SetPlayerArmour`
> yourself.

### `FakeBotStopCombat(botid)` / `FakeBotIsInCombat(botid)`

## Idle wander

### `FakeBotSetIdleWander(botid, bool:enabled, Float:radius = 10.0, Float:speed = 1.0)`
When enabled, a spawned bot with no other active route/follow/combat
command periodically walks to a random point within `radius` of where it
was when this was called, pauses, then picks a new point - useful for
ambient crowd filler with zero extra scripting.

### `FakeBotIsIdleWanderEnabled(botid)`

## Random appearance

### `FakeBotSetAppearancePool(const skinids[], count = sizeof(skinids))`
Overrides the built-in pool `FakeBotRandomizeAppearance()` picks from
(default: a conservative built-in set of universally valid pedestrian
skins). Call once, e.g. in `OnGameModeInit()`.

### `FakeBotRandomizeAppearance(botid)`
Applies a random skin from the pool and returns the chosen id (or `-1`).

## Bulk loading

### `FakeBotLoadFromFile(const filename[])`
Reads a JSON array of bot definitions from `filename` and creates them
all via `FakeBotCreate()` - a bulk-creation convenience, not a
replacement for the natives/callbacks API. Returns the number created.

```json
[
  {
    "name": "Patrol_Bot", "skin": 280,
    "x": 1958.33, "y": 1343.14, "z": 15.37, "angle": 0.0,
    "waypoints": [[1958.33, 1343.14, 15.37], [1975.02, 1343.96, 15.37]],
    "loop": true
  }
]
```
Every field except `name` is optional. `weapon`, `ammo`, `virtualworld`
and `interior` default to `0`; `waypoints`/`loop` are on-foot only.

## Performance / tuning

### `FakeBotSetTickRate(milliseconds)` / `FakeBotGetTickRate()`
Simulation tick interval (default 50ms / 20Hz). Higher values trade
movement smoothness for lower CPU usage on servers with many bots.
Refuses values under 10ms.

### `FakeBotSetPoolingEnabled(bool:enabled)` / `FakeBotIsPoolingEnabled()` / `FakeBotGetPooledCount()`
When enabled, `FakeBotDestroy()` hides a bot instead of disconnecting it
(no new RakNet handshake needed later), and `FakeBotCreate()` reuses a hidden
bot when one is available (it is renamed and re-spawned over its existing connection).
A pooled bot still occupies a player slot. Off by default.

### `FakeBotSetReservedSlots(slots)` / `FakeBotGetReservedSlots()`
Number of player slots bots must leave free for real players (default 1, minimum 1).
`FakeBotCreate()` returns -1 instead of taking one of them. One free slot is
mandatory: all bots connect from 127.0.0.1, and a server treats a burst of connections
from one address that takes the *last* slot as a "server full" attack and bans the address.

### `FakeBotSetNearbyRadius(Float:radius)`
Distance (GTA units, default 15.0) within which a real player triggers
`OnFakeBotPlayerNearby()` for a bot.

### `FakeBotSetFarUpdateDistance(Float:distance)`
Distance (default 60.0) beyond which a bot with no real player nearby has
its simulation rate throttled to save CPU - invisible to players, since
nobody is close enough to notice the lower update rate.

### `FakeBotGetLastTickTime()`
Milliseconds the last simulation tick took across every active bot -
useful for monitoring plugin overhead.

### `FakeBotGetMovingCount()` / `FakeBotGetDrivingCount()`
Live counts of bots currently walking/running/sprinting/following vs.
driving toward a target.

## Chat

### `FakeBotSendMessage(botid, const text[])`
Makes the bot "say" `text` in global chat exactly as a real player typing
in chat would: the message is run through every loaded script's real
`OnPlayerText(playerid, text)` handler first (so your existing chat
commands, filters and loggers all see it, with no special-casing needed
for bots), then broadcast as `"Name: text"` using the bot's own name and
chat color - unless a script explicitly returns `0` from `OnPlayerText`,
in which case the message is suppressed, matching standard SA-MP chat
behaviour for a blocked message.

## Groups

Groups are a lightweight, plugin-side bookkeeping convenience for
controlling many bots at once - a group has no behaviour of its own, it's
just a named set of bot ids that the `FakeBotGroup*` natives iterate over
for you.

### `FakeBotCreateGroup(groupid)` / `FakeBotDestroyGroup(groupid)` / `FakeBotIsValidGroup(groupid)`
`groupid` is any integer you choose - it is not allocated by the plugin.
Destroying a group only removes the grouping; member bots are untouched.

### `FakeBotAddToGroup(botid, groupid)` / `FakeBotRemoveFromGroup(botid, groupid)` / `FakeBotIsInGroup(botid, groupid)`
A bot may belong to any number of groups simultaneously.

### `FakeBotGetGroupCount(groupid)`

### `FakeBotGroupGoTo(groupid, Float:x, Float:y, Float:z, Float:speed = 1.0, movetype = FAKEBOTS_MOVE_WALK)`
Applies `FakeBotGoTo` to every bot in the group. Returns the number of
bots affected.

### `FakeBotGroupStopMoving(groupid)`

### `FakeBotGroupAddWaypoint(groupid, Float:x, Float:y, Float:z, Float:speed = 1.0, movetype = FAKEBOTS_MOVE_WALK)`

### `FakeBotGroupClearWaypoints(groupid)`

### `FakeBotGroupSetLoopWaypoints(groupid, bool:loop)`

### `FakeBotGroupDestroyBots(groupid)`
Destroys every bot currently in the group (the group itself remains,
now empty). Returns the number of bots destroyed.

### `FakeBotGroupSendMessage(groupid, const text[])`
Applies `FakeBotSendMessage` to every bot in the group.

### `FakeBotGroupFollow(groupid, leaderplayerid, Float:offsetx, Float:offsety, Float:speed = 2.2, Float:spacing = 1.5)`
Arranges the group into a staggered-line formation behind
`leaderplayerid` - bot N sits at `(offsetx, offsety - N*spacing)` in the
leader's local space and continuously re-follows as the leader moves.

## Localization

### `FakeBotSetLanguage(const lang[])`
Switches the active language, e.g. `FakeBotSetLanguage("fa")` loads
`FakeBots.fa.json`. `"en"` (or an empty string) always loads the default
`FakeBots.json`.

### `FakeBotGetLanguage(lang[], size = sizeof(lang))`

### `FakeBotReloadLanguage()`
Hot-reloads the active language file from disk.

### `FakeBotSetLanguageDirectory(const path[])`
Overrides the folder FakeBots looks in (default:
`plugins/FakeBots/lang`).

### `FakeBotGetText(const key[], dest[], size = sizeof(dest))`
Looks up `key`, falling back to the default English file, and finally to
`key` itself if nowhere defines it - so a typo or missing translation is
always visible rather than blank. See [Localization.md](Localization.md).

## Callbacks

All callbacks are optional; implement only what you need.

| Callback | Fires when |
|---|---|
| `OnFakeBotConnect(botid)` | Embedded RakNet client connection completed, player id assigned |
| `OnFakeBotSpawn(botid)` | Bot spawns into the world |
| `OnFakeBotDeath(botid, killerid, reason)` | Bot dies |
| `OnFakeBotDisconnect(botid, reason)` | Bot disconnects (destroy, kick, timeout, refused); `reason`: see below |
| `OnFakeBotReachDestination(botid, Float:x, Float:y, Float:z)` | On-foot route finished |
| `OnFakeBotWaypointReached(botid, waypointindex)` | Intermediate waypoint reached |
| `OnFakeBotEnterVehicle(botid, vehicleid, bool:ispassenger)` | Bot enters a vehicle |
| `OnFakeBotExitVehicle(botid, vehicleid)` | Bot exits a vehicle |
| `OnFakeBotVehicleReachDest(botid, vehicleid, Float:x, Float:y, Float:z)` | Driving route finished |
| `OnFakeBotStateChange(botid, newstate, oldstate)` | Player state transition (see `PLAYER_STATE_*`) |
| `OnFakeBotTakeDamage(botid, issuerid, Float:amount, weaponid, bodypart)` | Bot takes damage (open.mp only) |
| `OnFakeBotGiveDamage(botid, damagedid, Float:amount, weaponid, bodypart)` | Bot deals damage (open.mp only) |
| `OnFakeBotPlayerNearby(botid, playerid, Float:distance)` | A real player enters `FakeBotSetNearbyRadius()` of a bot (fires once per entry, not every tick) |

### `OnFakeBotDisconnect` reasons

| `reason` | Meaning |
|---|---|
| `0` | `FakeBotDestroy()` (or a normal close) |
| `29` | the server never answered the connection attempts |
| `31` | the server has no free slot |
| `32` | the server closed the connection (kick / ban / shutdown) |
| `33` | connection lost (timeout) |
| `36` | the server banned the address |
| `37` | wrong server password |
| `-2001` ... `-2004` | the server rejected the join: version, nickname (invalid or taken), modification, no slot |
| `-1001` ... `-1006` | a join step timed out (`-1000 - step`): 1 connect, 2 waiting for `InitGame`, 3 class, 4 spawn |
