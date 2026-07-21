#ifdef RE3_IN_SA

#include <d3d9.h>
#include "common.h"
#include "main.h"
#include "General.h"
#include "Draw.h"
#include "Sprite.h"
#include "Streaming.h"
#include "World.h"
#include "PlayerPed.h"
#include "Wanted.h"
#include "Weapon.h"
#include "ColPoint.h"
#include "EventList.h"
#include "Camera.h"
#include "Timer.h"
#include "Hud.h"
#include "Font.h"

extern void Re3Log(const char *fmt, ...);
extern void Re3_SetPortalInputEnabled(bool enabled);

struct Re3PortalState
{
	bool enabled;
	bool viewValid;
	bool preparePending;
	bool enterPending;
	bool prepared;
	bool playerVisibilitySaved;
	bool savedPlayerVisibility;
	bool playerProtectionSaved;
	bool savedBulletProof;
	bool savedFireProof;
	bool savedCollisionProof;
	bool savedMeleeProof;
	bool savedExplosionProof;
	bool savedCanBeDamaged;
	bool entered;
	bool returnRequested;
	bool havePreviousReturnDistance;
	bool returnPlaceKeyDown;
	CVector position;
	CVector right;
	CVector up;
	CVector forward;
	float fov;
	float viewWindowX;
	float viewWindowY;
	CVector preparePosition;
	float prepareHeading;
	CVector enterPosition;
	float enterHeading;
	CVector returnPosition;
	float returnYaw;
	float previousReturnDistance;
	CVector previousReturnSample;
	uint32 returnCooldownUntil;
	uint32 playerProtectionUntil;
	RwRaster *returnRaster;
	IDirect3DTexture9 *returnTexture;
	int32 returnTextureWidth;
	int32 returnTextureHeight;
	bool returnTextureCopyLogged;
	uint32 returnTextureCopyFailures;
};

static Re3PortalState gRe3Portal;

static bool
Re3PortalPlayerProtectionActive(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();
	const bool graceActive = gRe3Portal.playerProtectionUntil != 0 &&
		(int32)(gRe3Portal.playerProtectionUntil - now) > 0;
	return (gRe3Portal.enabled && !gRe3Portal.entered) || graceActive;
}

bool
Re3PortalBlocksPlayerArrest(void)
{
	return Re3PortalPlayerProtectionActive();
}

void
Re3PortalUpdatePlayerProtection(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;

	if(Re3PortalPlayerProtectionActive()){
		if(!gRe3Portal.playerProtectionSaved){
			gRe3Portal.playerProtectionSaved = true;
			gRe3Portal.savedBulletProof = player->bBulletProof;
			gRe3Portal.savedFireProof = player->bFireProof;
			gRe3Portal.savedCollisionProof = player->bCollisionProof;
			gRe3Portal.savedMeleeProof = player->bMeleeProof;
			gRe3Portal.savedExplosionProof = player->bExplosionProof;
			gRe3Portal.savedCanBeDamaged = player->m_bCanBeDamaged;
			Re3Log("Portal: preview player protection armed health=%.1f armour=%.1f wanted=%d",
				player->m_fHealth, player->m_fArmour,
				player->m_pWanted ? player->m_pWanted->GetWantedLevel() : -1);
		}
		player->bBulletProof = true;
		player->bFireProof = true;
		player->bCollisionProof = true;
		player->bMeleeProof = true;
		player->bExplosionProof = true;
		player->m_bCanBeDamaged = false;
		return;
	}

	if(gRe3Portal.playerProtectionSaved){
		player->bBulletProof = gRe3Portal.savedBulletProof;
		player->bFireProof = gRe3Portal.savedFireProof;
		player->bCollisionProof = gRe3Portal.savedCollisionProof;
		player->bMeleeProof = gRe3Portal.savedMeleeProof;
		player->bExplosionProof = gRe3Portal.savedExplosionProof;
		player->m_bCanBeDamaged = gRe3Portal.savedCanBeDamaged;
		gRe3Portal.playerProtectionSaved = false;
		gRe3Portal.playerProtectionUntil = 0;
		Re3Log("Portal: post-entry player protection released after 2000 ms");
	}
}

bool
Re3PortalShouldRenderHud(void)
{
	// During the SA-side preview this complete frame is sampled as a world
	// texture. Native GTA III HUD/menu/fade passes must only return once Claude
	// owns gameplay after crossing the portal.
	return !gRe3Portal.enabled || gRe3Portal.entered;
}

static float
Re3PortalProjectionFov(void)
{
	return Max(10.0f, Min(150.0f,
		RADTODEG(2.0f * Atan(Max(gRe3Portal.viewWindowX, 0.01f)))));
}

extern "C" __declspec(dllexport) void
Re3_PortalSetView(float px, float py, float pz,
	float rx, float ry, float rz,
	float ux, float uy, float uz,
	float fx, float fy, float fz,
	float fov, float viewWindowX, float viewWindowY)
{
	gRe3Portal.position = CVector(px, py, pz);
	gRe3Portal.right = CVector(rx, ry, rz);
	gRe3Portal.up = CVector(ux, uy, uz);
	gRe3Portal.forward = CVector(fx, fy, fz);
	gRe3Portal.fov = Max(10.0f, Min(150.0f, fov));
	gRe3Portal.viewWindowX = Max(viewWindowX, 0.01f);
	gRe3Portal.viewWindowY = Max(viewWindowY, 0.01f);
	gRe3Portal.viewValid = true;
}

extern "C" __declspec(dllexport) void
Re3_PortalPrepare(float x, float y, float z, float heading)
{
	CVector position(x, y, z);
	if(!gRe3Portal.prepared ||
	   (position - gRe3Portal.preparePosition).MagnitudeSqr() > 0.01f ||
	   Abs(heading - gRe3Portal.prepareHeading) > 0.001f){
		gRe3Portal.preparePosition = position;
		gRe3Portal.prepareHeading = heading;
		gRe3Portal.preparePending = true;
		gRe3Portal.prepared = false;
		gRe3Portal.returnPosition = position;
		gRe3Portal.returnYaw = heading;
		gRe3Portal.havePreviousReturnDistance = false;
	}
	Re3_SetPortalInputEnabled(false);
}

extern "C" __declspec(dllexport) void
Re3_PortalSetEnabled(int enabled)
{
	gRe3Portal.enabled = enabled != 0;
	if(gRe3Portal.enabled){
		gRe3Portal.entered = false;
		gRe3Portal.havePreviousReturnDistance = false;
		Re3_SetPortalInputEnabled(false);
	}else
		gRe3Portal.viewValid = false;
}

extern "C" __declspec(dllexport) void
Re3_PortalEnter(float x, float y, float z, float heading)
{
	gRe3Portal.enterPosition = CVector(x, y, z);
	gRe3Portal.enterHeading = heading;
	gRe3Portal.enterPending = true;
	gRe3Portal.preparePending = false;
	gRe3Portal.enabled = false;
	gRe3Portal.viewValid = false;
	gRe3Portal.playerProtectionUntil = CTimer::GetTimeInMilliseconds() + 2000;
}

static void
Re3PortalPlacePlayer(CPlayerPed *player, const CVector &position, float heading)
{
	player->Teleport(position);
	player->SetHeading(heading);
	player->m_fRotationCur = heading;
	player->m_fRotationDest = heading;
	player->SetMoveSpeed(0.0f, 0.0f, 0.0f);
}

static void
Re3PortalGrantPlayerLoadout(CPlayerPed *player)
{
	// The native GTA III weapon cheat uses GiveWeapon directly. Grant a compact
	// testing loadout on each real traversal and start on the pistol.
	player->GiveWeapon(WEAPONTYPE_COLT45, 500);
	player->GiveWeapon(WEAPONTYPE_UZI, 750);
	player->GiveWeapon(WEAPONTYPE_M16, 750);
	player->SetCurrentWeapon(WEAPONTYPE_COLT45);
}

void
Re3PortalProcessCommands(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;

	if(gRe3Portal.enterPending){
		gRe3Portal.enterPending = false;
		Re3PortalPlacePlayer(player, gRe3Portal.enterPosition, gRe3Portal.enterHeading);
		Re3PortalGrantPlayerLoadout(player);
		if(gRe3Portal.playerVisibilitySaved){
			player->bIsVisible = gRe3Portal.savedPlayerVisibility;
			gRe3Portal.playerVisibilitySaved = false;
		}else
			player->bIsVisible = true;
		TheCamera.RestoreWithJumpCut();
		CCam &cam = TheCamera.Cams[TheCamera.ActiveCam];
		CVector forward = gRe3Portal.forward;
		forward.Normalise();
		TheCamera.m_bCamDirectlyBehind = false;
		TheCamera.m_bCamDirectlyInFront = false;
		TheCamera.m_bUseTransitionBeta = true;
		cam.m_fTransitionBeta = CGeneral::GetATanOfXY(forward.x, forward.y) + PI;
		while(cam.m_fTransitionBeta >= PI) cam.m_fTransitionBeta -= 2.0f * PI;
		while(cam.m_fTransitionBeta < -PI) cam.m_fTransitionBeta += 2.0f * PI;
		cam.ResetStatics = true;
		cam.FOV = Re3PortalProjectionFov();
		TheCamera.Process();
		CTimer::EndUserPause();
		CTimer::SetCodePause(false);
		Re3_SetPortalInputEnabled(true);
		gRe3Portal.entered = true;
		gRe3Portal.playerProtectionUntil = CTimer::GetTimeInMilliseconds() + 2000;
		gRe3Portal.returnRequested = false;
		gRe3Portal.havePreviousReturnDistance = false;
		gRe3Portal.returnCooldownUntil = CTimer::GetTimeInMilliseconds() + 1000;
		Re3Log("Portal: enter applied pos=%.2f %.2f %.2f heading=%.3f protectionMs=2000",
			gRe3Portal.enterPosition.x, gRe3Portal.enterPosition.y,
			gRe3Portal.enterPosition.z, gRe3Portal.enterHeading);
		return;
	}

	if(gRe3Portal.preparePending){
		gRe3Portal.preparePending = false;
		// A zero destination means "use GTA III's native new-game spawn". This
		// avoids guessing map coordinates and gives PortalSA the exact live point
		// through Re3_PortalGetDestination on the following frame.
		if(gRe3Portal.preparePosition.MagnitudeSqr() < 1.0f){
			gRe3Portal.preparePosition = player->GetPosition();
			gRe3Portal.returnPosition = gRe3Portal.preparePosition;
			gRe3Portal.prepareHeading = player->m_fRotationCur;
			gRe3Portal.returnYaw = gRe3Portal.prepareHeading;
		}
		CStreaming::LoadScene(gRe3Portal.preparePosition);
		Re3PortalPlacePlayer(player, gRe3Portal.preparePosition, gRe3Portal.prepareHeading);
		CTimer::EndUserPause();
		CTimer::SetCodePause(false);
		gRe3Portal.prepared = true;
		Re3Log("Portal: destination prepared pos=%.2f %.2f %.2f heading=%.3f",
			gRe3Portal.preparePosition.x, gRe3Portal.preparePosition.y,
			gRe3Portal.preparePosition.z, gRe3Portal.prepareHeading);
	}
}

extern "C" __declspec(dllexport) int
Re3_PortalGetGameplayView(float *values)
{
	if(values == nil || !gRe3Portal.entered || Scene.camera == nil)
		return 0;
	RwMatrix *matrix = RwFrameGetLTM(RwCameraGetFrame(Scene.camera));
	if(matrix == nil)
		return 0;
	values[0] = matrix->pos.x; values[1] = matrix->pos.y; values[2] = matrix->pos.z;
	values[3] = matrix->right.x; values[4] = matrix->right.y; values[5] = matrix->right.z;
	values[6] = matrix->up.x; values[7] = matrix->up.y; values[8] = matrix->up.z;
	values[9] = matrix->at.x; values[10] = matrix->at.y; values[11] = matrix->at.z;
	values[12] = TheCamera.Cams[TheCamera.ActiveCam].FOV;
	const RwV2d *viewWindow = RwCameraGetViewWindow(Scene.camera);
	values[13] = viewWindow ? viewWindow->x : SCREEN_VIEWWINDOW;
	values[14] = viewWindow ? viewWindow->y : SCREEN_VIEWWINDOW / SCREEN_ASPECT_RATIO;
	return 1;
}

extern "C" __declspec(dllexport) int
Re3_PortalGetDestination(float *x, float *y, float *z, float *yaw)
{
	if(x == nil || y == nil || z == nil || yaw == nil)
		return 0;
	*x = gRe3Portal.returnPosition.x;
	*y = gRe3Portal.returnPosition.y;
	*z = gRe3Portal.returnPosition.z;
	*yaw = gRe3Portal.returnYaw;
	return gRe3Portal.prepared || gRe3Portal.entered ? 1 : 0;
}

extern "C" __declspec(dllexport) int
Re3_PortalConsumeReturnRequest(void)
{
	if(!gRe3Portal.returnRequested)
		return 0;
	gRe3Portal.returnRequested = false;
	gRe3Portal.entered = false;
	gRe3Portal.havePreviousReturnDistance = false;
	return 1;
}

extern "C" __declspec(dllexport) void
Re3_PortalSetReturnTexture(IDirect3DTexture9 *texture, int32 width, int32 height)
{
	if(texture == nil || width <= 0 || height <= 0){
		if(gRe3Portal.returnRaster)
			RwRasterDestroy(gRe3Portal.returnRaster);
		gRe3Portal.returnRaster = nil;
		gRe3Portal.returnTexture = nil;
		gRe3Portal.returnTextureWidth = 0;
		gRe3Portal.returnTextureHeight = 0;
		gRe3Portal.returnTextureCopyLogged = false;
		return;
	}

	const bool recreate = gRe3Portal.returnRaster == nil ||
		gRe3Portal.returnTextureWidth != width || gRe3Portal.returnTextureHeight != height;
	if(recreate && gRe3Portal.returnRaster){
		RwRasterDestroy(gRe3Portal.returnRaster);
		gRe3Portal.returnRaster = nil;
		gRe3Portal.returnTexture = nil;
	}
	if(gRe3Portal.returnRaster == nil){
		RwRaster *raster = RwRasterCreate(width, height, 32,
			rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT888);
		if(raster == nil)
			return;
		rw::d3d::D3dRaster *ext = GETD3DRASTEREXT(raster);
		if(ext == nil || ext->texture == nil){
			RwRasterDestroy(raster);
			return;
		}
		gRe3Portal.returnRaster = raster;
		gRe3Portal.returnTexture = reinterpret_cast<IDirect3DTexture9*>(ext->texture);
		gRe3Portal.returnTextureWidth = width;
		gRe3Portal.returnTextureHeight = height;
		gRe3Portal.returnTextureCopyLogged = false;
		gRe3Portal.returnTextureCopyFailures = 0;
	}

	IDirect3DSurface9 *source = nil;
	IDirect3DSurface9 *destination = nil;
	HRESULT hr = texture->GetSurfaceLevel(0, &source);
	if(SUCCEEDED(hr))
		hr = gRe3Portal.returnTexture->GetSurfaceLevel(0, &destination);
	if(SUCCEEDED(hr))
		hr = rw::d3d::d3ddevice->StretchRect(source, nil, destination, nil, D3DTEXF_NONE);
	if(source) source->Release();
	if(destination) destination->Release();
	if(SUCCEEDED(hr) && !gRe3Portal.returnTextureCopyLogged){
		gRe3Portal.returnTextureCopyLogged = true;
		Re3Log("Portal: San Andreas return texture copied %dx%d", width, height);
	}else if(FAILED(hr) && gRe3Portal.returnTextureCopyFailures++ < 5){
		Re3Log("Portal: San Andreas return texture copy failed hr=0x%08X", (uint32)hr);
	}
}

static eWeaponType
Re3PortalWeaponFromBridgeCode(int32 code)
{
	switch(code){
	case 0: return WEAPONTYPE_COLT45;
	case 1: return WEAPONTYPE_UZI;
	case 2: return WEAPONTYPE_AK47;
	case 3: return WEAPONTYPE_M16;
	case 4: return WEAPONTYPE_SNIPERRIFLE;
	default: return WEAPONTYPE_UNARMED;
	}
}

extern "C" __declspec(dllexport) int
Re3_PortalFireHitscan(float sx, float sy, float sz,
	float ex, float ey, float ez, int32 weaponCode)
{
	if(!gRe3Portal.enabled || !gRe3Portal.prepared || gRe3Portal.entered)
		return -1;

	CPlayerPed *shooter = FindPlayerPed();
	const eWeaponType weaponType = Re3PortalWeaponFromBridgeCode(weaponCode);
	if(shooter == nil || weaponType == WEAPONTYPE_UNARMED)
		return -1;

	CVector source(sx, sy, sz);
	CVector target(ex, ey, ez);
	const CVector shot = target - source;
	if(shot.MagnitudeSqr() < 0.0001f)
		return -1;

	CColPoint point{};
	CEntity *victim = nil;
	CEntity *oldIgnoreEntity = CWorld::pIgnoreEntity;
	const bool oldIncludeDeadPeds = CWorld::bIncludeDeadPeds;
	const bool oldIncludeCarTyres = CWorld::bIncludeCarTyres;
	CWorld::pIgnoreEntity = shooter;
	CWorld::bIncludeDeadPeds = true;
	CWorld::bIncludeCarTyres = true;
	const bool hit = CWeapon::ProcessLineOfSight(source, target, point, victim,
		weaponType, shooter, true, true, true, true, true, false, false);
	CWorld::pIgnoreEntity = oldIgnoreEntity;
	CWorld::bIncludeDeadPeds = oldIncludeDeadPeds;
	CWorld::bIncludeCarTyres = oldIncludeCarTyres;

	CVector2D ahead(shot.x, shot.y);
	const float aheadLength = Sqrt(ahead.x * ahead.x + ahead.y * ahead.y);
	if(aheadLength > 0.0001f){
		ahead.x /= aheadLength;
		ahead.y /= aheadLength;
	}else{
		ahead.x = 0.0f;
		ahead.y = 1.0f;
	}

	CEventList::RegisterEvent(EVENT_GUNSHOT, EVENT_ENTITY_PED, shooter, shooter, 1000);
	CWeapon::MakePedsJumpAtShot(shooter, &source, &target);
	CWeapon weapon;
	weapon.Initialise(weaponType, 1);
	weapon.DoBulletImpact(shooter, victim, &source, &target, &point, ahead);

	Re3Log("Portal: SA hitscan code=%d weapon=%d hit=%d entityType=%d model=%d impact=(%.2f %.2f %.2f)",
		weaponCode, (int)weaponType, hit ? 1 : 0,
		victim ? (int)victim->GetType() : -1,
		victim ? (int)victim->GetModelIndex() : -1,
		hit ? point.point.x : target.x,
		hit ? point.point.y : target.y,
		hit ? point.point.z : target.z);
	return hit ? 1 : 0;
}

void
Re3PortalApplyViewWindow(void)
{
	if(!gRe3Portal.enabled || !gRe3Portal.viewValid || Scene.camera == nil)
		return;
	RwV2d viewWindow = {gRe3Portal.viewWindowX, gRe3Portal.viewWindowY};
	RwCameraSetViewWindow(Scene.camera, &viewWindow);
}

void
Re3PortalApplyCamera(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(!gRe3Portal.enabled || !gRe3Portal.viewValid || Scene.camera == nil){
		if(player && gRe3Portal.playerVisibilitySaved){
			player->bIsVisible = gRe3Portal.savedPlayerVisibility;
			gRe3Portal.playerVisibilitySaved = false;
		}
		return;
	}
	if(player && !gRe3Portal.playerVisibilitySaved){
		gRe3Portal.savedPlayerVisibility = player->bIsVisible;
		gRe3Portal.playerVisibilitySaved = true;
	}
	if(player)
		player->bIsVisible = false;

	CVector up = gRe3Portal.up;
	CVector forward = gRe3Portal.forward;
	up.Normalise();
	forward.Normalise();
	CVector right = CrossProduct(up, forward);
	right.Normalise();
	up = CrossProduct(forward, right);
	up.Normalise();
	TheCamera.GetMatrix().GetRight() = right;
	TheCamera.GetMatrix().GetUp() = up;
	TheCamera.GetMatrix().GetForward() = forward;
	TheCamera.GetMatrix().GetPosition() = gRe3Portal.position;
	TheCamera.GetGameCamPosition() = gRe3Portal.position;
	CDraw::SetFOV(Re3PortalProjectionFov());
	TheCamera.CalculateDerivedValues();
	Re3PortalApplyViewWindow();

	RwFrame *frame = RwCameraGetFrame(Scene.camera);
	if(frame){
		RwMatrix *matrix = RwFrameGetMatrix(frame);
		*RwMatrixGetPos(matrix) = gRe3Portal.position;
		*RwMatrixGetRight(matrix) = right;
		*RwMatrixGetUp(matrix) = up;
		*RwMatrixGetAt(matrix) = forward;
		RwMatrixUpdate(matrix);
		RwFrameUpdateObjects(frame);
		RwFrameOrthoNormalize(frame);
	}
	RwCameraSetNearClipPlane(Scene.camera, 0.05f);
}

static CVector Re3PortalNormal(void)
{
	return CVector(Cos(gRe3Portal.returnYaw), Sin(gRe3Portal.returnYaw), 0.0f);
}

static CVector Re3PortalRight(void)
{
	return CVector(-Sin(gRe3Portal.returnYaw), Cos(gRe3Portal.returnYaw), 0.0f);
}

void
Re3PortalUpdateReturnPortal(void)
{
	if(!gRe3Portal.entered)
		return;
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;

	const bool keyDown = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
	const bool placeRequested = keyDown && !gRe3Portal.returnPlaceKeyDown;
	gRe3Portal.returnPlaceKeyDown = keyDown;
	if(placeRequested){
		CCam &cam = TheCamera.Cams[TheCamera.ActiveCam];
		CVector viewDirection = cam.Front;
		viewDirection.Normalise();
		CVector horizontalView(viewDirection.x, viewDirection.y, 0.0f);
		if(horizontalView.MagnitudeSqr() < 0.0001f)
			horizontalView = player->GetForward();
		horizontalView.z = 0.0f;
		horizontalView.Normalise();

		CVector position;
		CVector portalNormal = horizontalView * -1.0f;
		CColPoint hitPoint;
		CEntity *hitEntity = nil;
		CEntity *oldIgnoreEntity = CWorld::pIgnoreEntity;
		CWorld::pIgnoreEntity = player;
		const bool aimedAtSurface = CWorld::ProcessLineOfSight(
			cam.Source, cam.Source + viewDirection * 40.0f,
			hitPoint, hitEntity, true, false, false, true, true, true, false);
		CWorld::pIgnoreEntity = oldIgnoreEntity;
		if(aimedAtSurface){
			position = hitPoint.point;
			CVector surfaceNormal(hitPoint.normal.x, hitPoint.normal.y, 0.0f);
			if(surfaceNormal.MagnitudeSqr() > 0.16f){
				surfaceNormal.Normalise();
				portalNormal = surfaceNormal;
			}
			position += portalNormal * 0.10f;
		}else{
			position = player->GetPosition() + horizontalView * 4.0f;
		}
		bool found = false;
		const float ground = CWorld::FindGroundZFor3DCoord(position.x, position.y, position.z + 10.0f, &found);
		if(found)
			position.z = ground;
		gRe3Portal.returnPosition = position;
		gRe3Portal.returnYaw = CGeneral::GetATanOfXY(portalNormal.x, portalNormal.y);
		gRe3Portal.preparePosition = position;
		gRe3Portal.prepareHeading = gRe3Portal.returnYaw;
		gRe3Portal.prepared = true;
		gRe3Portal.havePreviousReturnDistance = false;
		gRe3Portal.returnCooldownUntil = CTimer::GetTimeInMilliseconds() + 750;
		Re3Log("Portal: return portal placed %.2f %.2f %.2f yaw=%.3f",
			position.x, position.y, position.z, gRe3Portal.returnYaw);
		static wchar placedMessage[64];
		AsciiToUnicode("Portal to San Andreas placed", placedMessage);
		CHud::SetHelpMessage(placedMessage, true);
	}

	CVector sample = player->GetPosition();
	sample.z += 1.0f;
	const CVector center = gRe3Portal.returnPosition + CVector(0.0f, 0.0f, 2.56f);
	const CVector relative = sample - center;
	const float distance = DotProduct(relative, Re3PortalNormal());
	if(gRe3Portal.havePreviousReturnDistance &&
	   CTimer::GetTimeInMilliseconds() >= gRe3Portal.returnCooldownUntil){
		const float previous = gRe3Portal.previousReturnDistance;
		const bool crossed = (previous > 0.01f && distance <= 0.0f) ||
			(previous < -0.01f && distance >= 0.0f);
		const float denominator = previous - distance;
		if(crossed && Abs(denominator) > 0.0001f){
			const float t = Max(0.0f, Min(1.0f, previous / denominator));
			const CVector hit = gRe3Portal.previousReturnSample +
				(sample - gRe3Portal.previousReturnSample) * t;
			const CVector local = hit - center;
			if(Abs(DotProduct(local, Re3PortalRight())) <= 2.15f && Abs(local.z) <= 2.60f){
				gRe3Portal.returnRequested = true;
				gRe3Portal.returnCooldownUntil = CTimer::GetTimeInMilliseconds() + 1500;
				Re3Log("Portal: GTA3->SA crossing requested");
			}
		}
	}
	gRe3Portal.previousReturnDistance = distance;
	gRe3Portal.previousReturnSample = sample;
	gRe3Portal.havePreviousReturnDistance = true;
}

static void
Re3PortalDrawQuad(const CVector *positions, RwRaster *raster, uint8 red, uint8 green, uint8 blue)
{
	RwIm3DVertex vertices[4];
	static const float u[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	static const float v[4] = {1.0f, 0.0f, 0.0f, 1.0f};
	for(int i = 0; i < 4; i++){
		RwIm3DVertexSetPos(&vertices[i], positions[i].x, positions[i].y, positions[i].z);
		RwIm3DVertexSetRGBA(&vertices[i], red, green, blue, 255);
		RwIm3DVertexSetU(&vertices[i], u[i]);
		RwIm3DVertexSetV(&vertices[i], v[i]);
	}
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, raster);
	RwImVertexIndex indices[6] = {0, 1, 2, 0, 2, 3};
	if(RwIm3DTransform(vertices, 4, nil, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA | rwIM3D_VERTEXUV)){
		if(raster == gRe3Portal.returnRaster && gRe3Portal.returnTexture && rw::d3d::d3ddevice)
			rw::d3d::d3ddevice->SetTexture(0, gRe3Portal.returnTexture);
		RwIm3DRenderIndexedPrimitive(rwPRIMTYPETRILIST, indices, 6);
		RwIm3DEnd();
	}
}

static bool
Re3PortalDrawScreenSpaceQuad(const CVector *positions, RwRaster *raster)
{
	if(raster == nil || Scene.camera == nil || SCREEN_WIDTH <= 0.0f || SCREEN_HEIGHT <= 0.0f)
		return false;
	RwIm2DVertex vertices[4];
	const float nearClip = CDraw::GetNearClipZ();
	const float farClip = CDraw::GetFarClipZ();
	const float nearScreenZ = RwIm2DGetNearScreenZ();
	const float farScreenZ = RwIm2DGetFarScreenZ();
	for(int i = 0; i < 4; i++){
		RwV3d screen;
		float spriteWidth, spriteHeight;
		if(!CSprite::CalcScreenCoors(positions[i], &screen, &spriteWidth, &spriteHeight, false) ||
		   screen.z <= nearClip || farClip <= nearClip)
			return false;
		const float depth = nearScreenZ + (screen.z - nearClip) *
			(farScreenZ - nearScreenZ) * farClip / ((farClip - nearClip) * screen.z);
		RwIm2DVertexSetScreenX(&vertices[i], screen.x);
		RwIm2DVertexSetScreenY(&vertices[i], screen.y);
		RwIm2DVertexSetScreenZ(&vertices[i], depth);
		RwIm2DVertexSetRecipCameraZ(&vertices[i], 1.0f);
		RwIm2DVertexSetIntRGBA(&vertices[i], 255, 255, 255, 255);
		RwIm2DVertexSetU(&vertices[i], screen.x / SCREEN_WIDTH, 1.0f);
		RwIm2DVertexSetV(&vertices[i], screen.y / SCREEN_HEIGHT, 1.0f);
	}
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, raster);
	if(gRe3Portal.returnTexture && rw::d3d::d3ddevice)
		rw::d3d::d3ddevice->SetTexture(0, gRe3Portal.returnTexture);
	RwImVertexIndex indices[6] = {0, 1, 2, 0, 2, 3};
	RwIm2DRenderIndexedPrimitive(rwPRIMTYPETRILIST, vertices, 4, indices, 6);
	return true;
}

void
Re3PortalRenderReturnPortal(void)
{
	if(!gRe3Portal.entered)
		return;
	const CVector normal = Re3PortalNormal();
	const CVector right = Re3PortalRight();
	const CVector up(0.0f, 0.0f, 1.0f);
	const CVector center = gRe3Portal.returnPosition + up * 2.56f + normal * 0.035f;
	const float outerW = 2.18f, outerH = 2.68f;
	const float innerW = 2.0f, innerH = 2.5f;
	CVector outer[4] = {
		center - right*outerW - up*outerH, center - right*outerW + up*outerH,
		center + right*outerW + up*outerH, center + right*outerW - up*outerH
	};
	CVector inner[4] = {
		center - right*innerW - up*innerH + normal*0.006f,
		center - right*innerW + up*innerH + normal*0.006f,
		center + right*innerW + up*innerH + normal*0.006f,
		center + right*innerW - up*innerH + normal*0.006f
	};
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLNONE);
	RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)rwTEXTUREADDRESSCLAMP);
	RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERLINEAR);
	Re3PortalDrawQuad(outer, nil, 255, 154, 24);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
	Re3PortalDrawScreenSpaceQuad(inner, gRe3Portal.returnRaster);
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nil);
}

#endif
