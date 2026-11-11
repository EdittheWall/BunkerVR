#pragma once

// Lets the game's creatures see the player at the real head's height.
//
// The engine functions the Beast, the rats and the soldiers use to see the
// player (cLuxAgent's four GetPlayerIsInLineOfSight variants and
// GetPlayerHeadPos) take the player's height from the game's body: standing or
// crouched, nothing in between. These hooks shift what they look at by how far
// the real head is above or below the game's eye, as the body script reports
// every update (cVR_BeastSight). The column's width, the feet and everything
// else stay as the game has them. If the body script hasn't reported for a
// moment, or says not to (knockdowns, ladders, the head inside a wall), the
// game's own values are used unchanged.

// Finds and hooks the seven engine functions, logging what it does. All or
// nothing: returns false if any one of them can't be found.
bool SightInstall();

// Called by the body script every update (cVR_BeastSight). `on`: the real head
// counts right now. `up`: how far it is above (+) or below (-) the game's eye,
// in game meters.
void SightFromScript(bool on, float up);
