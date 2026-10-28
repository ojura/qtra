#pragma once

// A kangaroo built from rounded cones.
//
// The body is never a mesh. A handful of numbers describe the pose (how far
// the legs are compressed, where the feet are swung, how the torso and tail
// are pitched, what the head is doing) and buildRig() turns them into about
// thirty-five rounded cones in the kangaroo's own frame. The renderer blends
// those into one smooth surface by ray marching their signed distance, so a
// thigh flows into the hip and the tail into the rump without any skinning.
//
// Frame: x forward, y up away from the ground, z to the kangaroo's right. The
// ground is y = 0 and the unit is one standing kangaroo, ear tips at about
// 1.05. The simulation scales it to the planet.
//
// Anatomy the proportions follow: a long hind foot that the animal stands on
// at rest and rises onto its toes to hop from; a knee that sits forward near
// the belly, so thigh and shin make a Z with the foot; a heavy pelvis and a
// narrow chest; short forearms; a thick tail that reaches the ground behind
// and works as a fifth leg at slow speed. Standard library only.

#include "kangaroo_math.h"

#include <array>
#include <cmath>

namespace kangaroo {

enum Material : int {
    MaterialFur = 0,
    MaterialPale = 1,
    MaterialDark = 2,
    MaterialEye = 3,
    MaterialFoot = 4,
    MaterialJoey = 5,
};

struct Primitive {
    V3 a;
    float ra = 0.0F;
    V3 b;
    float rb = 0.0F;
    int material = MaterialFur;
    // The smoothing radius used when this is merged with everything before it.
    // Zero is a hard union, which eyes and the nose want so they stay crisp.
    float blend = 0.0F;
};

struct Pose {
    float crouch = 0.0F;      // 0 resting height, 1 fully compressed on landing
    float heelLift = 0.0F;    // 0 standing on the whole foot, 1 up on the toes
    float airborne = 0.0F;    // 0 feet on the ground, 1 legs free to swing
    float legSwing = 0.0F;    // in the air: -1 trailing after push-off, +1 reaching to land
    float legExtend = 0.0F;   // 0 bent, 1 straightened at push-off
    float bodyPitch = 0.0F;   // radians, positive tips the torso forward
    float tailLift = 0.0F;    // 0 resting on the ground, 1 held out straight behind
    float headPitch = 0.0F;   // radians, positive lowers the head
    float headYaw = 0.0F;     // radians, positive turns to the right
    float armReachLeft = 0.0F;
    float armReachRight = 0.0F;
    float forepawsDown = 0.0F; // 0 forearms held up, 1 paws on the ground (the slow gait)
    float rear = 0.0F;        // 0 normal, 1 risen up onto the tail as when boxing
    float kick = 0.0F;        // 0 normal, 1 both hind feet thrown forward
    float lick = 0.0F;        // 0 normal, 1 forearm raised to the mouth
    float earLeft = 0.0F;     // radians of twitch
    float earRight = 0.0F;
    float blink = 0.0F;
    float joeyPeek = 0.0F;    // 0 hidden in the pouch, 1 head out
    float pouchFull = 0.0F;   // 0 flat, 1 a joey inside
    float juvenile = 0.0F;    // 0 adult proportions, 1 a young joey's
    // Which way is up as the animal feels it, in this frame. On a slope the
    // ground's normal is +y but gravity is not straight down, and the torso
    // leans to stay over its feet the way a person on a hillside stands
    // vertical rather than square to the ground.
    V3 feltUp{0.0F, 1.0F, 0.0F};
};

inline constexpr int maxPrimitives = 40;

struct Rig {
    std::array<Primitive, maxPrimitives> primitives{};
    int count = 0;
    V3 boundsMin;
    V3 boundsMax;
    // Where the body touches the ground, for footprints and dust.
    V3 toeLeft;
    V3 toeRight;
    V3 heelLeft;
    V3 heelRight;
    V3 tailTip;
    V3 pawLeft;
    V3 pawRight;
    V3 snout;
    V3 centreOfMass;

    void add(const V3& a, const float ra, const V3& b, const float rb,
             const int material, const float blend)
    {
        if (count < maxPrimitives) {
            primitives[count++] = Primitive{a, ra, b, rb, material, blend};
        }
    }
};

namespace rig_detail {

struct P2 {
    float x = 0.0F;
    float y = 0.0F;
};

inline P2 add(const P2& a, const P2& b) { return {a.x + b.x, a.y + b.y}; }
inline P2 sub(const P2& a, const P2& b) { return {a.x - b.x, a.y - b.y}; }
inline P2 mul(const P2& a, const float s) { return {a.x * s, a.y * s}; }
inline float len(const P2& a) { return std::sqrt(a.x * a.x + a.y * a.y); }
// Counter-clockwise in the x-y plane with x forward and y up, so a positive
// angle swings a downward-pointing limb forward.
inline P2 rot(const P2& v, const float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
inline P2 dir(const float angleFromHorizontal)
{
    return {std::cos(angleFromHorizontal), std::sin(angleFromHorizontal)};
}
inline P2 lerp2(const P2& a, const P2& b, const float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
inline V3 at(const P2& p, const float z) { return {p.x, p.y, z}; }

// Two-bone inverse kinematics in the sagittal plane. Returns the middle joint
// for a chain of lengths l1, l2 from root to target, bending to the side
// `bend` points to. A target out of reach is pulled in along the line, so the
// chain straightens instead of failing.
inline P2 twoBone(const P2& root, P2& target, const float l1, const float l2, const float bend)
{
    P2 d = sub(target, root);
    float distance = len(d);
    const float reach = l1 + l2 - 1e-4F;
    const float least = std::fabs(l1 - l2) + 1e-4F;
    if (distance > reach) {
        target = add(root, mul(d, reach / distance));
        d = sub(target, root);
        distance = reach;
    } else if (distance < least) {
        const P2 safe = distance > 1e-6F ? mul(d, 1.0F / distance) : P2{0.0F, -1.0F};
        target = add(root, mul(safe, least));
        d = sub(target, root);
        distance = least;
    }
    const P2 u = mul(d, 1.0F / distance);
    const float along = (l1 * l1 - l2 * l2 + distance * distance) / (2.0F * distance);
    const float across = std::sqrt(std::max(l1 * l1 - along * along, 0.0F));
    const P2 perpendicular{-u.y * bend, u.x * bend};
    return add(add(root, mul(u, along)), mul(perpendicular, across));
}

inline V3 cubicBezier(const V3& p0, const V3& p1, const V3& p2, const V3& p3, const float t)
{
    const float s = 1.0F - t;
    return p0 * (s * s * s) + p1 * (3.0F * s * s * t) + p2 * (3.0F * s * t * t) + p3 * (t * t * t);
}

} // namespace rig_detail

inline Rig buildRig(const Pose& pose)
{
    using namespace rig_detail;
    Rig rig;

    const float young = std::clamp(pose.juvenile, 0.0F, 1.0F);
    const float crouch = std::clamp(pose.crouch, 0.0F, 1.0F);
    const float extend = std::clamp(pose.legExtend, 0.0F, 1.0F);
    const float rear = std::clamp(pose.rear, 0.0F, 1.0F);
    const float kick = std::clamp(pose.kick, 0.0F, 1.0F);
    const float airborne = std::clamp(pose.airborne, 0.0F, 1.0F);
    const float paws = std::clamp(pose.forepawsDown, 0.0F, 1.0F);

    // Limb lengths. A joey's legs are shorter for its height and its head is
    // larger, which is most of what reads as "young".
    const float thighLength = 0.205F * (1.0F - 0.08F * young);
    const float shinLength = 0.255F * (1.0F - 0.1F * young);
    const float footLength = 0.27F * (1.0F - 0.06F * young);
    const float headScale = 1.0F + 0.32F * young;

    // ---- pelvis ---------------------------------------------------------
    // Rest height 0.34. Landing compresses it, push-off and rearing raise it.
    // Rearing onto the tail lifts the hips high enough that the feet can come
    // off the ground for a kick, which is what the tail is for.
    const P2 hip{-0.02F + 0.035F * extend - 0.04F * rear,
                 0.34F - 0.09F * crouch + 0.07F * extend + 0.17F * rear};

    // ---- hind legs --------------------------------------------------------
    // On the ground the toe stays put and the heel rises; in the air the whole
    // leg swings about the hip. Both are computed and blended by `airborne`
    // so a take-off is continuous.
    const float heelAngle = 0.08F + 0.78F * std::clamp(pose.heelLift, 0.0F, 1.0F);
    const P2 groundToe{0.15F - 0.05F * crouch + 0.02F * extend, 0.014F};
    const P2 groundHock = add(groundToe, mul(dir(pi - heelAngle), footLength));

    const float swing = std::clamp(pose.legSwing, -1.0F, 1.0F);
    const float swingAngle = 0.62F * swing;
    const float legSpan = (thighLength + shinLength) * (0.78F + 0.2F * extend);
    const P2 airHock = add(hip, mul(rot(P2{-0.12F, -1.0F}, swingAngle), legSpan / 1.007F));
    const float footAngle = -2.05F + 1.7F * (swing + 1.0F) * 0.5F;
    const P2 airToe = add(airHock, mul(dir(footAngle), footLength));

    const P2 kickHock = add(hip, P2{0.17F, -0.16F});
    const P2 kickToe = add(kickHock, mul(dir(0.32F), footLength));

    P2 hock = lerp2(groundHock, airHock, airborne);
    P2 toe = lerp2(groundToe, airToe, airborne);
    hock = lerp2(hock, kickHock, kick);
    toe = lerp2(toe, kickToe, kick);
    {
        // Keep the toe at the foot's length from wherever the hock ended up.
        const P2 footDir = sub(toe, hock);
        const float l = len(footDir);
        if (l > 1e-5F) {
            toe = add(hock, mul(footDir, footLength / l));
        }
    }
    P2 reachedHock = hock;
    const P2 knee = twoBone(hip, reachedHock, thighLength, shinLength, 1.0F);
    if (reachedHock.x != hock.x || reachedHock.y != hock.y) {
        // Out of reach: the foot comes with the hock, as a heel lifting would.
        const P2 shift = sub(reachedHock, hock);
        hock = reachedHock;
        toe = add(toe, shift);
    }

    // ---- torso, from the pelvis up ---------------------------------------
    // Rest is about 30 degrees off vertical. Rearing brings it upright.
    const float torsoAngle = 0.52F + pose.bodyPitch - 0.62F * rear;
    const P2 pelvis = add(hip, P2{-0.03F, 0.025F});
    const P2 chest = add(pelvis, mul(P2{std::sin(torsoAngle), std::cos(torsoAngle)}, 0.31F));

    const float headDown = std::clamp(pose.headPitch, -0.6F, 1.6F);
    const float neckAngle = 0.22F + 0.45F * pose.bodyPitch + 1.05F * headDown - 0.3F * rear;
    const P2 neckTop = add(chest, mul(P2{std::sin(neckAngle), std::cos(neckAngle)}, 0.15F));
    // The snout points slightly down at rest and follows headPitch, while the
    // neck absorbs part of the torso's pitch so the head stays nearly level
    // through a hop, as a hopping kangaroo's does.
    const float snoutAngle = -0.32F - 0.85F * headDown + 0.15F * rear;

    // ---- the upper body in 3D, before the lean -----------------------------
    // Everything above the hip turns about it toward feltUp. Built in a
    // lambda so the lean is applied in one place.
    const V3 up = normalize(pose.feltUp);
    float leanPitch = std::atan2(up.x, std::max(up.y, 0.2F));
    float leanRoll = std::atan2(up.z, std::max(up.y, 0.2F));
    leanPitch = std::clamp(leanPitch, -0.7F, 0.7F);
    leanRoll = std::clamp(leanRoll, -0.6F, 0.6F);
    const V3 hip3 = at(hip, 0.0F);
    const auto lean = [&](const V3& p) {
        V3 q = p - hip3;
        // Pitch: about z, tipping +y toward +x when leanPitch is positive.
        q = rotate(q, V3{0.0F, 0.0F, -1.0F}, leanPitch);
        // Roll: about x, tipping +y toward +z.
        q = rotate(q, V3{1.0F, 0.0F, 0.0F}, leanRoll);
        return q + hip3;
    };

    const float bodyBlend = 0.055F;
    const float limbBlend = 0.03F;

    // Points placed along the torso, so the belly, the pouch and a peeking
    // joey turn with it when the animal pitches forward to graze. `along` is
    // measured up the torso's axis from the pelvis and `out` forward of it.
    const P2 torsoAxis{std::sin(torsoAngle), std::cos(torsoAngle)};
    const P2 torsoFront{std::cos(torsoAngle), -std::sin(torsoAngle)};
    const auto onTorso = [&](const float along, const float out) {
        return add(pelvis, add(mul(torsoAxis, along), mul(torsoFront, out)));
    };

    // Torso: a heavy pelvis tapering to a narrow chest, and a paler belly set
    // forward of it so the front reads lighter.
    rig.add(lean(at(pelvis, 0.0F)), 0.158F, lean(at(chest, 0.0F)), 0.098F, MaterialFur, 0.0F);
    rig.add(lean(at(onTorso(0.02F, 0.072F), 0.0F)), 0.115F, lean(at(onTorso(0.27F, 0.045F), 0.0F)), 0.07F,
            MaterialPale, 0.03F);

    // Pouch, fuller with a joey in it.
    const float full = std::clamp(pose.pouchFull, 0.0F, 1.0F);
    const P2 pouch = onTorso(0.02F, 0.09F + 0.03F * full);
    if (full > 0.0F) {
        rig.add(lean(at(pouch, 0.0F)), 0.085F + 0.03F * full, lean(at(pouch, 0.0F)),
                0.085F + 0.03F * full, MaterialPale, 0.05F);
    }

    // Neck and head.
    rig.add(lean(at(chest, 0.0F)), 0.072F, lean(at(neckTop, 0.0F)), 0.05F * headScale, MaterialFur,
            bodyBlend);
    const V3 headPivot = lean(at(neckTop, 0.0F));
    // The head turns about the top of the neck: yaw about the felt up, pitch
    // already in snoutAngle.
    const auto head = [&](const P2& offset, const float z) {
        const P2 p = mul(rot(offset, snoutAngle + 0.32F), headScale);
        V3 q{p.x, p.y, z * headScale};
        q = rotate(q, V3{0.0F, 1.0F, 0.0F}, -pose.headYaw);
        q = rotate(q, V3{0.0F, 0.0F, -1.0F}, leanPitch);
        q = rotate(q, V3{1.0F, 0.0F, 0.0F}, leanRoll);
        return headPivot + q;
    };
    // Skull to snout. Offsets are from the top of the neck in a frame where
    // the snout points along -0.32 rad, so the rotation above is relative.
    const P2 skullBack{0.0F, 0.05F};
    const P2 skullFront{0.075F, 0.035F};
    const P2 snoutTip{0.16F, -0.015F};
    rig.add(head(skullBack, 0.0F), 0.066F, head(skullFront, 0.0F), 0.054F, MaterialFur, limbBlend);
    rig.add(head(skullFront, 0.0F), 0.047F, head(snoutTip, 0.0F), 0.027F, MaterialFur, 0.025F);
    rig.add(head(add(snoutTip, P2{0.012F, 0.004F}), 0.0F), 0.021F,
            head(add(snoutTip, P2{0.012F, 0.004F}), 0.0F), 0.021F, MaterialDark, 0.008F);
    rig.snout = head(add(snoutTip, P2{0.025F, 0.0F}), 0.0F);

    const float eyeRadius = 0.0155F * (1.0F - 0.8F * std::clamp(pose.blink, 0.0F, 1.0F));
    for (const float side : {-1.0F, 1.0F}) {
        const V3 eye = head(P2{0.085F, 0.062F}, 0.04F * side);
        rig.add(eye, eyeRadius, eye, eyeRadius, MaterialEye, 0.0F);
    }

    // Ears: tall, pointing up and back, twitching independently.
    for (const float side : {-1.0F, 1.0F}) {
        const float twitch = side < 0.0F ? pose.earLeft : pose.earRight;
        const V3 base = head(P2{0.0F, 0.1F}, 0.032F * side);
        P2 tipOffset{-0.06F - 0.04F * twitch, 0.235F - 0.03F * std::fabs(twitch)};
        const V3 tip = head(tipOffset, (0.065F + 0.03F * twitch) * side);
        const V3 tipDark = lerp(base, tip, 0.72F);
        rig.add(base, 0.03F, tipDark, 0.016F, MaterialFur, 0.02F);
        rig.add(tipDark, 0.016F, tip, 0.006F, MaterialDark, 0.012F);
    }

    // Forearms. Rest is folded in front of the chest; a reach is a jab, paws
    // down plants them on the ground for the slow five-legged gait, and lick
    // brings one up to the mouth, which is how kangaroos cool themselves.
    const float upperArm = 0.12F;
    const float forearm = 0.115F;
    rig.pawLeft = V3{};
    rig.pawRight = V3{};
    for (const float side : {-1.0F, 1.0F}) {
        const float reach = std::clamp(side < 0.0F ? pose.armReachLeft : pose.armReachRight, 0.0F, 1.0F);
        const P2 shoulder = add(chest, P2{0.01F, -0.035F});
        P2 paw = add(shoulder, P2{0.075F, -0.15F});
        paw = lerp2(paw, add(shoulder, P2{0.225F, 0.015F}), reach);
        // Paws to the ground: the target is on y = 0 ahead of the chest. The
        // chain reaches as far as it can, and the body pitch that comes with
        // this gait is what lets it get there.
        const P2 ground{chest.x + 0.1F, 0.012F};
        paw = lerp2(paw, ground, paws);
        if (side > 0.0F) {
            paw = lerp2(paw, add(neckTop, P2{0.08F, -0.03F}), std::clamp(pose.lick, 0.0F, 1.0F));
        }
        P2 reached = paw;
        const P2 elbow = twoBone(shoulder, reached, upperArm, forearm, -1.0F);
        const float z = 0.075F * side;
        rig.add(lean(at(shoulder, z)), 0.034F, lean(at(elbow, z * 1.08F)), 0.026F, MaterialFur, limbBlend);
        rig.add(lean(at(elbow, z * 1.08F)), 0.026F, lean(at(reached, z * 0.85F)), 0.019F, MaterialFur, 0.02F);
        const V3 pawPoint = lean(at(add(reached, P2{0.012F, -0.004F}), z * 0.85F));
        rig.add(pawPoint, 0.017F, pawPoint, 0.017F, MaterialDark, 0.012F);
        (side < 0.0F ? rig.pawLeft : rig.pawRight) = pawPoint;
    }

    // Hind legs: thigh, shin, foot. These are not leaned; the feet are on the
    // ground and the hip is where the lean pivots.
    for (const float side : {-1.0F, 1.0F}) {
        const float z = 0.085F * side;
        const V3 hip3d = at(hip, z);
        const V3 knee3d = at(knee, z * 1.08F);
        const V3 hock3d = at(hock, z * 0.88F);
        const V3 toe3d = at(toe, z * 0.82F);
        rig.add(hip3d, 0.102F, knee3d, 0.058F, MaterialFur, bodyBlend);
        rig.add(knee3d, 0.046F, hock3d, 0.03F, MaterialFur, limbBlend);
        rig.add(hock3d, 0.032F, toe3d, 0.019F, MaterialFoot, 0.022F);
        if (side < 0.0F) {
            rig.toeLeft = toe3d;
            rig.heelLeft = hock3d;
        } else {
            rig.toeRight = toe3d;
            rig.heelRight = hock3d;
        }
    }

    // Tail: a cubic curve from the rump. At rest it lies along the ground
    // behind; lifted it is held out straight as a counterweight; rearing it
    // is planted under the body as a prop.
    {
        const float lift = std::clamp(pose.tailLift, 0.0F, 1.2F);
        const V3 base = lean(at(add(pelvis, P2{-0.11F, -0.03F}), 0.0F));
        const float tailLength = 0.74F * (1.0F - 0.15F * young);
        const V3 restTip{base.x - tailLength * 0.97F, 0.026F, 0.0F};
        const V3 liftedTip{base.x - tailLength * 0.94F, base.y + 0.06F + 0.16F * (lift - 1.0F), 0.0F};
        const V3 proppedTip{base.x - tailLength * 0.62F, 0.026F, 0.0F};
        V3 tip = lerp(restTip, liftedTip, std::min(lift, 1.0F));
        tip = lerp(tip, proppedTip, rear);
        tip.y = std::max(tip.y, 0.022F);
        const V3 c1 = base + V3{-0.16F, -0.1F * (1.0F - std::min(lift, 1.0F)) - 0.12F * rear, 0.0F};
        const V3 c2 = lerp(base, tip, 0.65F) + V3{0.0F, -0.02F * (1.0F - lift), 0.0F};
        const std::array<float, 5> radii{0.088F, 0.068F, 0.05F, 0.033F, 0.014F};
        V3 previous = base;
        for (int i = 1; i < 5; ++i) {
            const V3 point = cubicBezier(base, c1, c2, tip, static_cast<float>(i) / 4.0F);
            rig.add(previous, radii[i - 1], point, radii[i], MaterialFur, i == 1 ? bodyBlend : 0.02F);
            previous = point;
        }
        rig.tailTip = tip;
    }

    // A joey's head at the pouch opening, sliding out as it peeks.
    const float peek = std::clamp(pose.joeyPeek, 0.0F, 1.0F);
    if (peek > 0.02F && full > 0.0F) {
        const P2 opening = onTorso(0.11F + 0.03F * peek, 0.1F + 0.07F * peek);
        const V3 back = lean(at(opening, 0.0F));
        const V3 mouth = back + V3{0.032F, 0.004F, 0.0F};
        const V3 nose = mouth + V3{0.05F, -0.012F, 0.0F};
        rig.add(back, 0.04F, mouth, 0.034F, MaterialJoey, 0.012F);
        rig.add(mouth, 0.028F, nose, 0.016F, MaterialJoey, 0.012F);
        rig.add(nose, 0.011F, nose, 0.011F, MaterialDark, 0.004F);
        for (const float side : {-1.0F, 1.0F}) {
            rig.add(back + V3{0.0F, 0.03F, 0.018F * side}, 0.016F,
                    back + V3{-0.025F, 0.09F, 0.032F * side}, 0.006F, MaterialJoey, 0.01F);
            const V3 eye = mouth + V3{0.012F, 0.022F, 0.021F * side};
            rig.add(eye, 0.008F, eye, 0.008F, MaterialEye, 0.0F);
        }
    }

    // In the air the legs hang below the hip and the frame's origin is the
    // point the simulation flies, which leaves and meets the ground at the
    // toes. Raising the body by however far the lowest foot hangs below that
    // origin puts the toes on the ground at push-off and touchdown, and lets
    // the body ride above the trajectory in between.
    if (airborne > 0.0F) {
        const float lowest = std::min({rig.toeLeft.y, rig.toeRight.y, rig.heelLeft.y, rig.heelRight.y});
        const float raise = std::max(0.014F - lowest, 0.0F) * airborne;
        const V3 shift{0.0F, raise, 0.0F};
        for (int i = 0; i < rig.count; ++i) {
            rig.primitives[i].a += shift;
            rig.primitives[i].b += shift;
        }
        for (V3* point : {&rig.toeLeft, &rig.toeRight, &rig.heelLeft, &rig.heelRight, &rig.tailTip,
                          &rig.pawLeft, &rig.pawRight, &rig.snout}) {
            *point += shift;
        }
    }

    // Bounds for the ray marcher, and a centre of mass for tumbling.
    V3 lo{1e9F, 1e9F, 1e9F};
    V3 hi{-1e9F, -1e9F, -1e9F};
    V3 sum{};
    float weight = 0.0F;
    for (int i = 0; i < rig.count; ++i) {
        const Primitive& p = rig.primitives[i];
        for (int axis = 0; axis < 3; ++axis) {
            lo[axis] = std::min({lo[axis], p.a[axis] - p.ra, p.b[axis] - p.rb});
            hi[axis] = std::max({hi[axis], p.a[axis] + p.ra, p.b[axis] + p.rb});
        }
        const float w = p.ra * p.ra * p.ra + p.rb * p.rb * p.rb;
        sum += (p.a + p.b) * (0.5F * w);
        weight += w;
    }
    // Smooth blending swells the surface by up to the blend radius.
    const V3 margin{0.06F, 0.06F, 0.06F};
    rig.boundsMin = lo - margin;
    rig.boundsMax = hi + margin;
    rig.centreOfMass = weight > 0.0F ? sum / weight : V3{0.0F, 0.4F, 0.0F};
    return rig;
}

} // namespace kangaroo
