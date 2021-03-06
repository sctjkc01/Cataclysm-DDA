#include "item.h"
#include "creature.h"
#include "output.h"
#include "game.h"
#include "map.h"
#include "messages.h"
#include "rng.h"
#include "translations.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "itype.h"
#include "vehicle.h"
#include "debug.h"

#include <algorithm>
#include <numeric>
#include <cmath>
#include <map>

const efftype_id effect_blind( "blind" );
const efftype_id effect_bounced( "bounced" );
const efftype_id effect_downed( "downed" );
const efftype_id effect_onfire( "onfire" );
const efftype_id effect_sap( "sap" );
const efftype_id effect_sleep( "sleep" );
const efftype_id effect_stunned( "stunned" );
const efftype_id effect_zapped( "zapped" );
const efftype_id effect_lying_down( "lying_down" );

static std::map<int, std::map<body_part, double> > default_hit_weights = {
    {
        -1, /* attacker smaller */
        {   { bp_eyes, 0.f },
            { bp_head, 0.f },
            { bp_torso, 20.f },
            { bp_arm_l, 15.f },
            { bp_arm_r, 15.f },
            { bp_leg_l, 25.f },
            { bp_leg_r, 25.f }
        }
    },
    {
        0, /* attacker equal size */
        {   { bp_eyes, 0.33f },
            { bp_head, 2.33f },
            { bp_torso, 33.33f },
            { bp_arm_l, 20.f },
            { bp_arm_r, 20.f },
            { bp_leg_l, 12.f },
            { bp_leg_r, 12.f }
        }
    },
    {
        1, /* attacker larger */
        {   { bp_eyes, 0.57f },
            { bp_head, 5.71f },
            { bp_torso, 36.57f },
            { bp_arm_l, 22.86f },
            { bp_arm_r, 22.86f },
            { bp_leg_l, 5.71f },
            { bp_leg_r, 5.71f }
        }
    }
};

struct weight_compare {
    bool operator() (const std::pair<body_part, double> &left,
                     const std::pair<body_part, double> &right)
    {
        return left.second < right.second;
    }
};

const std::map<std::string, m_size> Creature::size_map = {
    {"TINY", MS_TINY}, {"SMALL", MS_SMALL}, {"MEDIUM", MS_MEDIUM},
    {"LARGE", MS_LARGE}, {"HUGE", MS_HUGE} };

Creature::Creature()
{
    moves = 0;
    pain = 0;
    killer = NULL;
    speed_base = 100;
    underwater = false;

    reset_bonuses();

    fake = false;
}

Creature::~Creature()
{
}

void Creature::normalize()
{
}

void Creature::reset()
{
    reset_bonuses();
    reset_stats();
}
void Creature::reset_bonuses()
{
    num_blocks = 1;
    num_dodges = 1;
    num_blocks_bonus = 0;
    num_dodges_bonus = 0;

    armor_bash_bonus = 0;
    armor_cut_bonus = 0;

    speed_bonus = 0;
    dodge_bonus = 0;
    block_bonus = 0;
    hit_bonus = 0;
    bash_bonus = 0;
    cut_bonus = 0;

    bash_mult = 1.0f;
    cut_mult = 1.0f;

    melee_quiet = false;
    grab_resist = 0;
    throw_resist = 0;
}

void Creature::reset_stats()
{
    // "Creatures" have no stats!
    // This only exists to simplify cleanup
    // TODO: Make this not exist
}

void Creature::process_turn()
{
    if(is_dead_state()) {
        return;
    }
    reset_bonuses();

    process_effects();

    // Call this in case any effects have changed our stats
    reset_stats();

    // add an appropriate number of moves
    moves += get_speed();
}

// MF_DIGS or MF_CAN_DIG and diggable terrain
bool Creature::digging() const
{
    return false;
}

bool Creature::sees( const Creature &critter ) const
{
    if( critter.is_hallucination() ) {
        // hallucinations are imaginations of the player character, npcs or monsters don't hallucinate.
        // Invisible hallucinations would be pretty useless (nobody would see them at all), therefor
        // the player will see them always.
        return is_player();
    }

    const auto p = dynamic_cast< const player* >( &critter );
    if( p != nullptr && p->is_invisible() ) {
        // Let invisible players see themselves (simplifies drawing)
        return p == this;
    }

    if( !fov_3d && !debug_mode && posz() != critter.posz() ) {
        return false;
    }

    const int wanted_range = rl_dist( pos(), critter.pos() );
    if( wanted_range <= 1 &&
        ( posz() == critter.posz() || g->m.valid_move( pos(), critter.pos(), false, true ) ) ) {
        return true;
    } else if( ( wanted_range > 1 && critter.digging() ) ||
        (critter.has_flag(MF_NIGHT_INVISIBILITY) && g->m.light_at(critter.pos()) <= LL_LOW ) ||
        ( critter.is_underwater() && !is_underwater() && g->m.is_divable( critter.pos() ) ) ) {
        return false;
    }

    return sees( critter.pos(), critter.is_player() );
}

bool Creature::sees( const int tx, const int ty ) const
{
    return sees( tripoint( tx, ty, posz() ) );
}

bool Creature::sees( const point t ) const
{
    return sees( tripoint( t, posz() ) );
}

bool Creature::sees( const tripoint &t, bool is_player ) const
{
    if( !fov_3d && posz() != t.z ) {
        return false;
    }

    const int range_cur = sight_range( g->m.ambient_light_at(t) );
    const int range_day = sight_range( DAYLIGHT_LEVEL );
    const int range_min = std::min( range_cur, range_day );
    const int wanted_range = rl_dist( pos(), t );
    if( wanted_range <= range_min ||
        ( wanted_range <= range_day &&
          g->m.ambient_light_at( t ) > g->natural_light_level( t.z ) ) ) {
        int range = 0;
        if( g->m.ambient_light_at( t ) > g->natural_light_level( t.z ) ) {
            range = wanted_range;
        } else {
            range = range_min;
        }
        if( is_player ) {
            // Special case monster -> player visibility, forcing it to be symmetric with player vision.
            return range >= wanted_range &&
                g->m.get_cache_ref(pos().z).seen_cache[pos().x][pos().y] > LIGHT_TRANSPARENCY_SOLID;
        } else {
            return g->m.sees( pos(), t, range );
        }
    } else {
        return false;
    }
}

// Helper function to check if potential area of effect of a weapon overlaps vehicle
// Maybe TODO: If this is too slow, precalculate a bounding box and clip the tested area to it
bool overlaps_vehicle( const std::set<tripoint> &veh_area, const tripoint &pos, const int area )
{
    tripoint tmp = pos;
    int &x = tmp.x;
    int &y = tmp.y;
    for( x = pos.x - area; x < pos.x + area; x++ ) {
        for( y = pos.y - area; y < pos.y + area; y++ ) {
            if( veh_area.count( tmp ) > 0 ) {
                return true;
            }
        }
    }

    return false;
}

Creature *Creature::auto_find_hostile_target( int range, int &boo_hoo, int area )
{
    Creature *target = nullptr;
    player &u = g->u; // Could easily protect something that isn't the player
    constexpr int hostile_adj = 2; // Priority bonus for hostile targets
    const int iff_dist = ( range + area ) * 3 / 2 + 6; // iff check triggers at this distance
    int iff_hangle = 15 + area; // iff safety margin (degrees). less accuracy, more paranoia
    float best_target_rating = -1.0f; // bigger is better
    int u_angle = 0;         // player angle relative to turret
    boo_hoo = 0;         // how many targets were passed due to IFF. Tragically.
    bool self_area_iff = false; // Need to check if the target is near the vehicle we're a part of
    bool area_iff = false;      // Need to check distance from target to player
    bool angle_iff = true;      // Need to check if player is in a cone between us and target
    int pldist = rl_dist( pos(), g->u.pos() );
    int part;
    vehicle *in_veh = is_fake() ? g->m.veh_at( pos(), part ) : nullptr;
    if( pldist < iff_dist && sees( g->u ) ) {
        area_iff = area > 0;
        angle_iff = true;
        // Player inside vehicle won't be hit by shots from the roof,
        // so we can fire "through" them just fine.
        if( in_veh && g->m.veh_at( u.pos(), part ) == in_veh && in_veh->is_inside( part ) ) {
            angle_iff = false; // No angle IFF, but possibly area IFF
        } else if( pldist < 3 ) {
            iff_hangle = (pldist == 2 ? 30 : 60);    // granularity increases with proximity
        }
        u_angle = g->m.coord_to_angle(posx(), posy(), u.posx(), u.posy());
    }

    if( area > 0 && in_veh != nullptr ) {
        self_area_iff = true;
    }

    std::vector<Creature*> targets;
    targets.reserve( g->num_zombies() + g->active_npc.size() );
    for (size_t i = 0; i < g->num_zombies(); i++) {
        monster &m = g->zombie(i);
        if( m.friendly != 0 ) {
            // friendly to the player, not a target for us
            continue;
        }
        targets.push_back( &m );
    }
    for( auto &p : g->active_npc ) {
        if( p->attitude != NPCATT_KILL ) {
            // friendly to the player, not a target for us
            continue;
        }
        targets.push_back( p );
    }
    for( auto &m : targets ) {
        if( !sees( *m ) ) {
            // can't see nor sense it
            continue;
        }
        int dist = rl_dist( pos(), m->pos() ) + 1; // rl_dist can be 0
        if( dist > range + 1 || dist < area ) {
            // Too near or too far
            continue;
        }
        // Prioritize big, armed and hostile stuff
        float mon_rating = m->power_rating();
        float target_rating = mon_rating / dist;
        if( mon_rating + hostile_adj <= 0 ) {
            // We wouldn't attack it even if it was hostile
            continue;
        }

        if( in_veh != nullptr && g->m.veh_at( m->pos(), part ) == in_veh ) {
            // No shooting stuff on vehicle we're a part of
            continue;
        }
        if( area_iff && rl_dist( u.pos(), m->pos() ) <= area ) {
            // Player in AoE
            boo_hoo++;
            continue;
        }
        // Hostility check can be expensive, but we need to inform the player of boo_hoo
        // only when the target is actually "hostile enough"
        bool maybe_boo = false;
        if( angle_iff ) {
            int tangle = g->m.coord_to_angle(posx(), posy(), m->posx(), m->posy());
            int diff = abs(u_angle - tangle);
            // Player is in the angle and not too far behind the target
            if( ( diff + iff_hangle > 360 || diff < iff_hangle ) &&
                ( dist * 3 / 2 + 6 > pldist ) ) {
                maybe_boo = true;
            }
        }
        if( !maybe_boo && ( ( mon_rating + hostile_adj ) / dist <= best_target_rating ) ) {
            // "Would we skip the target even if it was hostile?"
            // Helps avoid (possibly expensive) attitude calculation
            continue;
        }
        if( m->attitude_to( u ) == A_HOSTILE ) {
            target_rating = ( mon_rating + hostile_adj ) / dist;
            if( maybe_boo ) {
                boo_hoo++;
                continue;
            }
        }
        if( target_rating <= best_target_rating || target_rating <= 0 ) {
            continue; // Handle this late so that boo_hoo++ can happen
        }
        // Expensive check for proximity to vehicle
        if( self_area_iff && overlaps_vehicle( in_veh->get_points(), m->pos(), area ) ) {
            continue;
        }

        target = m;
        best_target_rating = target_rating;
    }
    return target;
}

void Creature::melee_attack(Creature &t, bool allow_special)
{
    static const matec_id no_technique_id( "" );
    melee_attack( t, allow_special, no_technique_id );
}

/*
 * Damage-related functions
 */

int Creature::deal_melee_attack( Creature *source, int hitroll )
{
    int hit_spread = hitroll - dodge_roll();

    // If attacker missed call targets on_dodge event
    if( hit_spread <= 0 && !source->is_hallucination() ) {
        on_dodge( source, source->get_melee() );
    }

    return hit_spread;
}

void Creature::deal_melee_hit(Creature *source, int hit_spread, bool critical_hit,
                              const damage_instance &dam, dealt_damage_instance &dealt_dam)
{
    damage_instance d = dam; // copy, since we will mutate in block_hit

    body_part bp_hit = select_body_part(source, hit_spread);
    block_hit(source, bp_hit, d);

    // Bashing crit
    if( critical_hit && !is_immune_effect( effect_stunned ) ) {
        if( d.type_damage(DT_BASH) * hit_spread > get_hp_max() ) {
            add_effect( effect_stunned, 1 ); // 1 turn is enough
        }
    }

    // Stabbing effects
    int stab_moves = rng( d.type_damage(DT_STAB) / 2,
                          d.type_damage(DT_STAB) * 1.5 );
    if (critical_hit) {
        stab_moves *= 1.5;
    }
    if( stab_moves >= 150 && !is_immune_effect( effect_downed ) ) {
        if( is_player() ) {
            source->add_msg_if_npc( m_bad, _("<npcname> forces you to the ground!"));
        } else {
            source->add_msg_player_or_npc( m_good, _("You force %s to the ground!"),
                                           _("<npcname> forces %s to the ground!"),
                                           disp_name().c_str() );
        }

        add_effect( effect_downed, 1);
        mod_moves(-stab_moves / 2);
    } else {
        mod_moves(-stab_moves);
    }

    on_hit( source, bp_hit ); // trigger on-gethit events
    dealt_dam = deal_damage(source, bp_hit, d);
    dealt_dam.bp_hit = bp_hit;
}

/**
 * Attempts to harm a creature with a projectile.
 *
 * @param source Pointer to the creature who shot the projectile.
 * @param attack A structure describing the attack and its results.
 */
void Creature::deal_projectile_attack( Creature *source, dealt_projectile_attack &attack )
{
    const double missed_by = attack.missed_by;
    if( missed_by >= 1.0 ) {
        // Total miss
        return;
    }

    const projectile &proj = attack.proj;
    dealt_damage_instance &dealt_dam = attack.dealt_dam;
    const auto &proj_effects = proj.proj_effects;

    const bool u_see_this = g->u.sees(*this);

    const int avoid_roll = dodge_roll();
    // Do dice(10, speed) instead of dice(speed, 10) because speed could potentially be > 10000
    const int diff_roll = dice( 10, proj.speed );
    // Partial dodge, capped at [0.0, 1.0], added to missed_by
    const double dodge_rescaled = avoid_roll / static_cast<double>( diff_roll );
    const double goodhit = missed_by + std::max( 0.0, std::min( 1.0, dodge_rescaled ) ) ;

    if( goodhit >= 1.0 ) {
        // "Avoid" rather than "dodge", because it includes removing self from the line of fire
        //  rather than just Matrix-style bullet dodging
        if( source != nullptr && g->u.sees( *source ) ) {
            add_msg_player_or_npc(
                m_warning,
                _("You avoid %s projectile!"),
                _("<npcname> avoids %s projectile."),
                source->disp_name(true).c_str() );
        } else {
            add_msg_player_or_npc(
                m_warning,
                _("You avoid an incoming projectile!"),
                _("<npcname> avoids an incoming projectile.") );
        }

        attack.missed_by = 1.0; // Arbitrary value
        return;
    }

    // Bounce applies whether it does damage or not.
    if( proj.proj_effects.count( "BOUNCE" ) ) {
        add_effect( effect_bounced, 1);
    }

    body_part bp_hit;
    double hit_value = missed_by + rng_float(-0.5, 0.5);
    // Headshots considered elsewhere
    if( hit_value <= 0.4 ) {
        bp_hit = bp_torso;
    } else if (one_in(4)) {
        if( one_in(2)) {
            bp_hit = bp_leg_l;
        } else {
            bp_hit = bp_leg_r;
        }
    } else {
        if( one_in(2)) {
            bp_hit = bp_arm_l;
        } else {
            bp_hit = bp_arm_r;
        }
    }

    double damage_mult = 1.0;

    std::string message = "";
    game_message_type gmtSCTcolor = m_neutral;

    if( goodhit < 0.1 ) {
        message = _("Headshot!");
        gmtSCTcolor = m_headshot;
        damage_mult *= rng_float(2.45, 3.35);
        bp_hit = bp_head; // headshot hits the head, of course
    } else if( goodhit < 0.2 ) {
        message = _("Critical!");
        gmtSCTcolor = m_critical;
        damage_mult *= rng_float(1.75, 2.3);
    } else if( goodhit < 0.4 ) {
        message = _("Good hit!");
        gmtSCTcolor = m_good;
        damage_mult *= rng_float(1, 1.5);
    } else if( goodhit < 0.6 ) {
        damage_mult *= rng_float(0.5, 1);
    } else if( goodhit < 0.8 ) {
        message = _("Grazing hit.");
        gmtSCTcolor = m_grazing;
        damage_mult *= rng_float(0, .25);
    } else {
        damage_mult *= 0;
    }

    if( source != nullptr && !message.empty() ) {
        source->add_msg_if_player(m_good, message.c_str());
    }

    attack.missed_by = goodhit;

    // copy it, since we're mutating
    damage_instance impact = proj.impact;
    if( proj_effects.count("NOGIB") > 0 ) {
        impact.add_effect("NOGIB");
    }
    if( damage_mult > 0.0f && proj_effects.count( "NO_DAMAGE_SCALING" ) ) {
        damage_mult = 1.0f;
    }

    impact.mult_damage(damage_mult);

    dealt_dam = deal_damage(source, bp_hit, impact);
    dealt_dam.bp_hit = bp_hit;

    // Apply ammo effects to target.
    const std::string target_material = get_material();
    if (proj.proj_effects.count("FLAME")) {
        if (0 == target_material.compare("veggy") || 0 == target_material.compare("cotton") ||
            0 == target_material.compare("wool") || 0 == target_material.compare("paper") ||
            0 == target_material.compare("wood" ) ) {
            add_effect( effect_onfire, rng(8, 20));
        } else if (0 == target_material.compare("flesh") || 0 == target_material.compare("iflesh") ) {
            add_effect( effect_onfire, rng(5, 10));
        }
    } else if (proj.proj_effects.count("INCENDIARY") ) {
        if (0 == target_material.compare("veggy") || 0 == target_material.compare("cotton") ||
            0 == target_material.compare("wool") || 0 == target_material.compare("paper") ||
            0 == target_material.compare("wood") ) {
            add_effect( effect_onfire, rng(2, 6));
        } else if ( (0 == target_material.compare("flesh") || 0 == target_material.compare("iflesh") ) &&
                    one_in(4) ) {
            add_effect( effect_onfire, rng(1, 4));
        }
    } else if (proj.proj_effects.count("IGNITE")) {
        if (0 == target_material.compare("veggy") || 0 == target_material.compare("cotton") ||
            0 == target_material.compare("wool") || 0 == target_material.compare("paper") ||
            0 == target_material.compare("wood") ) {
            add_effect( effect_onfire, rng(6, 6));
        } else if (0 == target_material.compare("flesh") || 0 == target_material.compare("iflesh") ) {
            add_effect( effect_onfire, rng(10, 10));
        }
    }

    if( bp_hit == bp_head && proj_effects.count( "BLINDS_EYES" ) ) {
        // TODO: Change this to require bp_eyes
        add_env_effect( effect_blind, bp_eyes, 5, rng( 3, 10 ) );
    }

    if( proj_effects.count( "APPLY_SAP" ) ) {
        add_effect( effect_sap, dealt_dam.total_damage() );
    }

    int stun_strength = 0;
    if (proj.proj_effects.count("BEANBAG")) {
        stun_strength = 4;
    }
    if (proj.proj_effects.count("LARGE_BEANBAG")) {
        stun_strength = 16;
    }
    if( stun_strength > 0 ) {
        switch( get_size() ) {
        case MS_TINY:
            stun_strength *= 4;
            break;
        case MS_SMALL:
            stun_strength *= 2;
            break;
        case MS_MEDIUM:
        default:
            break;
        case MS_LARGE:
            stun_strength /= 2;
            break;
        case MS_HUGE:
            stun_strength /= 4;
            break;
        }
        add_effect( effect_stunned, rng(stun_strength / 2, stun_strength) );
    }

    if(u_see_this) {
        if( damage_mult == 0 ) {
            if( source != nullptr ) {
                add_msg( source->is_player() ? _("You miss!") : _("The shot misses!") );
            }
        } else if( dealt_dam.total_damage() == 0 ) {
            //~ 1$ - monster name, 2$ - character's bodypart or monster's skin/armor
            add_msg( _("The shot reflects off %1$s %2$s!"), disp_name(true).c_str(),
                     is_monster() ?
                        skin_name().c_str() :
                        body_part_name_accusative(bp_hit).c_str() );
        } else if( is_player() ) {
                //monster hits player ranged
                //~ Hit message. 1$s is bodypart name in accusative. 2$d is damage value.
                add_msg_if_player(m_bad, _( "You were hit in the %1$s for %2$d damage." ),
                                  body_part_name_accusative(bp_hit).c_str(),
                                  dealt_dam.total_damage());
        } else if( source != nullptr ) {
            if( source->is_player() ) {
                //player hits monster ranged
                SCT.add(posx(), posy(),
                        direction_from(0, 0, posx() - source->posx(), posy() - source->posy()),
                        get_hp_bar(dealt_dam.total_damage(), get_hp_max(), true).first,
                        m_good, message, gmtSCTcolor);

                if (get_hp() > 0) {
                    SCT.add(posx(), posy(),
                            direction_from(0, 0, posx() - source->posx(), posy() - source->posy()),
                            get_hp_bar(get_hp(), get_hp_max(), true).first, m_good,
                            //~ "hit points", used in scrolling combat text
                            _("hp"), m_neutral, "hp");
                } else {
                    SCT.removeCreatureHP();
                }

                add_msg(m_good, _("You hit %s for %d damage."),
                        disp_name().c_str(), dealt_dam.total_damage());
            } else if( u_see_this ) {
                //~ 1$ - shooter, 2$ - target
                add_msg(_("%1$s shoots %2$s."),
                        source->disp_name().c_str(), disp_name().c_str());
            }
        }
    }
    check_dead_state();
    attack.hit_critter = this;
    attack.missed_by = goodhit;
}

dealt_damage_instance Creature::deal_damage(Creature *source, body_part bp,
        const damage_instance &dam)
{
    if( is_dead_state() ) {
        return dealt_damage_instance();
    }
    int total_damage = 0;
    int total_pain = 0;
    damage_instance d = dam; // copy, since we will mutate in absorb_hit

    std::vector<int> dealt_dams(NUM_DT, 0);

    absorb_hit(bp, d);

    // add up all the damage units dealt
    int cur_damage;
    for (std::vector<damage_unit>::const_iterator it = d.damage_units.begin();
         it != d.damage_units.end(); ++it) {
        cur_damage = 0;
        deal_damage_handle_type(*it, bp, cur_damage, total_pain);
        if (cur_damage > 0) {
            dealt_dams[it->type] += cur_damage;
            total_damage += cur_damage;
        }
    }

    mod_pain(total_pain);
    if( dam.effects.count("NOGIB") ) {
        total_damage = std::min( total_damage, get_hp() + 1 );
    }

    apply_damage(source, bp, total_damage);
    return dealt_damage_instance(dealt_dams);
}
void Creature::deal_damage_handle_type(const damage_unit &du, body_part, int &damage, int &pain)
{
    // Handles ACIDPROOF, electric immunity etc.
    if( is_immune_damage( du.type ) ) {
        return;
    }

    // Apply damage multiplier from skill, critical hits or grazes after all other modifications.
    const int adjusted_damage = du.amount * du.damage_multiplier;
    switch (du.type) {
    case DT_BASH:
        damage += adjusted_damage;
        // add up pain before using mod_pain since certain traits modify that
        pain += adjusted_damage / 4;
        mod_moves(-rng(0, damage * 2)); // bashing damage reduces moves
        break;
    case DT_CUT:
        damage += adjusted_damage;
        pain += (adjusted_damage + sqrt(double(adjusted_damage))) / 4;
        break;
    case DT_STAB: // stab differs from cut in that it ignores some armor
        damage += adjusted_damage;
        pain += (adjusted_damage + sqrt(double(adjusted_damage))) / 4;
        break;
    case DT_HEAT: // heat damage sets us on fire sometimes
        damage += adjusted_damage;
        pain += adjusted_damage / 4;
        if( rng(0, 100) < adjusted_damage ) {
            add_effect( effect_onfire, rng(1, 3));
        }
        break;
    case DT_ELECTRIC: // Electrical damage adds a major speed/dex debuff
        damage += adjusted_damage;
        pain += adjusted_damage / 4;
        add_effect( effect_zapped, std::max( adjusted_damage, 2 ) );
        break;
    case DT_COLD: // cold damage slows us a bit and hurts less
        damage += adjusted_damage;
        pain += adjusted_damage / 6;
        mod_moves(-adjusted_damage * 80);
        break;
    case DT_ACID: // Acid damage and acid burns are super painful
        damage += adjusted_damage;
        pain += adjusted_damage / 3;
        break;
    default:
        damage += adjusted_damage;
        pain += adjusted_damage / 4;
    }
}

/*
 * State check functions
 */

bool Creature::is_warm() const
{
    return true;
}

bool Creature::is_fake() const
{
    return fake;
}

void Creature::set_fake(const bool fake_value)
{
    fake = fake_value;
}

/*
 * Effect-related methods
 */
bool Creature::move_effects(bool attacking)
{
    (void)attacking;
    return true;
}

void Creature::add_eff_effects(effect e, bool reduced)
{
    (void)e;
    (void)reduced;
    return;
}

void Creature::add_effect( const efftype_id &eff_id, int dur, body_part bp,
                           bool permanent, int intensity, bool force )
{
    // Check our innate immunity
    if( !force && is_immune_effect( eff_id ) ) {
        return;
    }

    if( !eff_id.is_valid() ) {
        debugmsg( "Invalid effect, ID: %s", eff_id.c_str() );
        return;
    }
    const effect_type &type = eff_id.obj();

    // Mutate to a main (HP'd) body_part if necessary.
    if (type.get_main_parts()) {
        bp = mutate_to_main_part(bp);
    }

    bool found = false;
    // Check if we already have it
    auto matching_map = effects.find(eff_id);
    if (matching_map != effects.end()) {
        auto &bodyparts = matching_map->second;
        auto found_effect = bodyparts.find(bp);
        if (found_effect != bodyparts.end()) {
            found = true;
            effect &e = found_effect->second;
            // If we do, mod the duration, factoring in the mod value
            e.mod_duration(dur * e.get_dur_add_perc() / 100);
            // Limit to max duration
            if (e.get_max_duration() > 0 && e.get_duration() > e.get_max_duration()) {
                e.set_duration(e.get_max_duration());
            }
            // Adding a permanent effect makes it permanent
            if( e.is_permanent() ) {
                e.pause_effect();
            }
            // Set intensity if value is given
            if (intensity > 0) {
                e.set_intensity(intensity);
            // Else intensity uses the type'd step size if it already exists
            } else if (e.get_int_add_val() != 0) {
                e.mod_intensity(e.get_int_add_val());
            }

            // Bound intensity by [1, max intensity]
            if (e.get_intensity() < 1) {
                add_msg( m_debug, "Bad intensity, ID: %s", e.get_id().c_str() );
                e.set_intensity(1);
            } else if (e.get_intensity() > e.get_max_intensity()) {
                e.set_intensity(e.get_max_intensity());
            }
        }
    }

    if( found == false ) {
        // If we don't already have it then add a new one

        // Then check if the effect is blocked by another
        for( auto &elem : effects ) {
            for( auto &_effect_it : elem.second ) {
                for( const auto blocked_effect : _effect_it.second.get_blocks_effects() ) {
                    if (blocked_effect == eff_id) {
                        // The effect is blocked by another, return
                        return;
                    }
                }
            }
        }

        // Now we can make the new effect for application
        effect e(&type, dur, bp, permanent, intensity, calendar::turn);
        // Bound to max duration
        if (e.get_max_duration() > 0 && e.get_duration() > e.get_max_duration()) {
            e.set_duration(e.get_max_duration());
        }

        // Force intensity if it is duration based
        if( e.get_int_dur_factor() != 0 ) {
            // + 1 here so that the lowest is intensity 1, not 0
             e.set_intensity( ( e.get_duration() / e.get_int_dur_factor() ) + 1 );
        }
        // Bound new effect intensity by [1, max intensity]
        if (e.get_intensity() < 1) {
            add_msg( m_debug, "Bad intensity, ID: %s", e.get_id().c_str() );
            e.set_intensity(1);
        } else if (e.get_intensity() > e.get_max_intensity()) {
            e.set_intensity(e.get_max_intensity());
        }
        effects[eff_id][bp] = e;
        if (is_player()) {
            // Only print the message if we didn't already have it
            if(type.get_apply_message() != "") {
                     add_msg(type.gain_game_message_type(),
                             _(type.get_apply_message().c_str()));
            }
            add_memorial_log(pgettext("memorial_male",
                                           type.get_apply_memorial_log().c_str()),
                                  pgettext("memorial_female",
                                           type.get_apply_memorial_log().c_str()));
        }
        // Perform any effect addition effects.
        bool reduced = resists_effect(e);
        add_eff_effects(e, reduced);
    }
}
bool Creature::add_env_effect( const efftype_id &eff_id, body_part vector, int strength, int dur,
                               body_part bp, bool permanent, int intensity, bool force )
{
    if( !force && is_immune_effect( eff_id ) ) {
        return false;
    }

    if (dice(strength, 3) > dice(get_env_resist(vector), 3)) {
        // Only add the effect if we fail the resist roll
        // Don't check immunity (force == true), because we did check above
        add_effect( eff_id, dur, bp, permanent, intensity, true );
        return true;
    } else {
        return false;
    }
}
void Creature::clear_effects()
{
    effects.clear();
}
bool Creature::remove_effect( const efftype_id &eff_id, body_part bp )
{
    if (!has_effect(eff_id, bp)) {
        //Effect doesn't exist, so do nothing
        return false;
    }
    const effect_type &type = eff_id.obj();

    if (is_player()) {
        // Print the removal message and add the memorial log if needed
        if(type.get_remove_message() != "") {
            add_msg(type.lose_game_message_type(),
                         _(type.get_remove_message().c_str()));
        }
        add_memorial_log(pgettext("memorial_male",
                                       type.get_remove_memorial_log().c_str()),
                              pgettext("memorial_female",
                                       type.get_remove_memorial_log().c_str()));
    }

    // num_bp means remove all of a given effect id
    if (bp == num_bp) {
        effects.erase(eff_id);
    } else {
        effects[eff_id].erase(bp);
        // If there are no more effects of a given type remove the type map
        if (effects[eff_id].empty()) {
            effects.erase(eff_id);
        }
    }
    return true;
}
bool Creature::has_effect( const efftype_id &eff_id, body_part bp ) const
{
    // num_bp means anything targeted or not
    if (bp == num_bp) {
        return effects.find( eff_id ) != effects.end();
    } else {
        auto got_outer = effects.find(eff_id);
        if(got_outer != effects.end()) {
            auto got_inner = got_outer->second.find(bp);
            if (got_inner != got_outer->second.end()) {
                return true;
            }
        }
        return false;
    }
}

effect &Creature::get_effect( const efftype_id &eff_id, body_part bp )
{
    return const_cast<effect &>( const_cast<const Creature*>(this)->get_effect( eff_id, bp ) );
}

const effect &Creature::get_effect( const efftype_id &eff_id, body_part bp ) const
{
    auto got_outer = effects.find(eff_id);
    if(got_outer != effects.end()) {
        auto got_inner = got_outer->second.find(bp);
        if (got_inner != got_outer->second.end()) {
            return got_inner->second;
        }
    }
    return effect::null_effect;
}
int Creature::get_effect_dur( const efftype_id &eff_id, body_part bp ) const
{
    const effect &eff = get_effect(eff_id, bp);
    if( !eff.is_null() ) {
        return eff.get_duration();
    }

    return 0;
}
int Creature::get_effect_int( const efftype_id &eff_id, body_part bp ) const
{
    const effect &eff = get_effect(eff_id, bp);
    if( !eff.is_null() ) {
        return eff.get_intensity();
    }

    return 0;
}
void Creature::process_effects()
{
    // id's and body_part's of all effects to be removed. If we ever get player or
    // monster specific removals these will need to be moved down to that level and then
    // passed in to this function.
    std::vector<efftype_id> rem_ids;
    std::vector<body_part> rem_bps;

    // Decay/removal of effects
    for( auto &elem : effects ) {
        for( auto &_it : elem.second ) {
            // Add any effects that others remove to the removal list
            for( const auto removed_effect : _it.second.get_removes_effects() ) {
                rem_ids.push_back( removed_effect );
                rem_bps.push_back(num_bp);
            }
            // Run decay effects, marking effects for removal as necessary.
            _it.second.decay( rem_ids, rem_bps, calendar::turn, is_player() );
        }
    }

    // Actually remove effects. This should be the last thing done in process_effects().
    for (size_t i = 0; i < rem_ids.size(); ++i) {
        remove_effect( rem_ids[i], rem_bps[i] );
    }
}

bool Creature::resists_effect(effect e)
{
    for (auto &i : e.get_resist_effects()) {
        if (has_effect(i)) {
            return true;
        }
    }
    for (auto &i : e.get_resist_traits()) {
        if (has_trait(i)) {
            return true;
        }
    }
    return false;
}

bool Creature::has_trait(const std::string &flag) const
{
    (void)flag;
    return false;
}

// Methods for setting/getting misc key/value pairs.
void Creature::set_value( const std::string key, const std::string value )
{
    values[ key ] = value;
}

void Creature::remove_value( const std::string key )
{
    values.erase( key );
}

std::string Creature::get_value( const std::string key ) const
{
    auto it = values.find( key );
    return ( it == values.end() ) ? "" : it->second;
}

void Creature::mod_pain(int npain)
{
    pain += npain;
    // Pain should never go negative
    if (pain < 0) {
        pain = 0;
    }
}
void Creature::mod_moves(int nmoves)
{
    moves += nmoves;
}
void Creature::set_moves(int nmoves)
{
    moves = nmoves;
}

bool Creature::in_sleep_state() const
{
    return has_effect( effect_sleep ) || has_effect( effect_lying_down );
}

/*
 * Killer-related things
 */
Creature *Creature::get_killer() const
{
    return killer;
}

void Creature::set_killer( Creature * const killer )
{
    // Only the first killer will be stored, calling set_killer again with a different
    // killer would mean it's called on a dead creature and therefor ignored.
    if( killer != nullptr && !killer->is_fake() && this->killer == nullptr ) {
        this->killer = killer;
    }
}

int Creature::get_num_blocks() const
{
    return num_blocks + num_blocks_bonus;
}
int Creature::get_num_dodges() const
{
    return num_dodges + num_dodges_bonus;
}
int Creature::get_num_blocks_bonus() const
{
    return num_blocks_bonus;
}
int Creature::get_num_dodges_bonus() const
{
    return num_dodges_bonus;
}

// currently this is expected to be overridden to actually have use
int Creature::get_env_resist(body_part) const
{
    return 0;
}
int Creature::get_armor_bash(body_part) const
{
    return armor_bash_bonus;
}
int Creature::get_armor_cut(body_part) const
{
    return armor_cut_bonus;
}
int Creature::get_armor_bash_base(body_part) const
{
    return armor_bash_bonus;
}
int Creature::get_armor_cut_base(body_part) const
{
    return armor_cut_bonus;
}
int Creature::get_armor_bash_bonus() const
{
    return armor_bash_bonus;
}
int Creature::get_armor_cut_bonus() const
{
    return armor_cut_bonus;
}

int Creature::get_speed() const
{
    return get_speed_base() + get_speed_bonus();
}
int Creature::get_dodge() const
{
    return get_dodge_base() + get_dodge_bonus();
}
int Creature::get_melee() const
{
    return 0;
}
int Creature::get_hit() const
{
    return get_hit_base() + get_hit_bonus();
}

int Creature::get_speed_base() const
{
    return speed_base;
}
int Creature::get_dodge_base() const
{
    return 0;
}
int Creature::get_hit_base() const
{
    return 0;
}
int Creature::get_speed_bonus() const
{
    return speed_bonus;
}
int Creature::get_dodge_bonus() const
{
    return dodge_bonus;
}
int Creature::get_block_bonus() const
{
    return block_bonus; //base is 0
}
int Creature::get_hit_bonus() const
{
    return hit_bonus; //base is 0
}
int Creature::get_bash_bonus() const
{
    return bash_bonus;
}
int Creature::get_cut_bonus() const
{
    return cut_bonus;
}

float Creature::get_bash_mult() const
{
    return bash_mult;
}
float Creature::get_cut_mult() const
{
    return cut_mult;
}

bool Creature::get_melee_quiet() const
{
    return melee_quiet;
}
int Creature::get_grab_resist() const
{
    return grab_resist;
}

int Creature::get_throw_resist() const
{
    return throw_resist;
}

void Creature::mod_stat( const std::string &stat, int modifier )
{
    if( stat == "speed" ) {
        mod_speed_bonus( modifier );
    } else if( stat == "dodge" ) {
        mod_dodge_bonus( modifier );
    } else if( stat == "block" ) {
        mod_block_bonus( modifier );
    } else if( stat == "hit" ) {
        mod_hit_bonus( modifier );
    } else if( stat == "bash" ) {
        mod_bash_bonus( modifier );
    } else if( stat == "cut" ) {
        mod_cut_bonus( modifier );
    } else if( stat == "pain" ) {
        mod_pain( modifier );
    } else if( stat == "moves" ) {
        mod_moves( modifier );
    } else {
        add_msg( "Tried to modify a nonexistent stat %s.", stat.c_str() );
    }
}


void Creature::set_num_blocks_bonus(int nblocks)
{
    num_blocks_bonus = nblocks;
}
void Creature::set_num_dodges_bonus(int ndodges)
{
    num_dodges_bonus = ndodges;
}

void Creature::set_armor_bash_bonus(int nbasharm)
{
    armor_bash_bonus = nbasharm;
}
void Creature::set_armor_cut_bonus(int ncutarm)
{
    armor_cut_bonus = ncutarm;
}

void Creature::set_speed_base(int nspeed)
{
    speed_base = nspeed;
}
void Creature::set_speed_bonus(int nspeed)
{
    speed_bonus = nspeed;
}
void Creature::set_dodge_bonus(int ndodge)
{
    dodge_bonus = ndodge;
}
void Creature::set_block_bonus(int nblock)
{
    block_bonus = nblock;
}
void Creature::set_hit_bonus(int nhit)
{
    hit_bonus = nhit;
}
void Creature::set_bash_bonus(int nbash)
{
    bash_bonus = nbash;
}
void Creature::set_cut_bonus(int ncut)
{
    cut_bonus = ncut;
}
void Creature::mod_speed_bonus(int nspeed)
{
    speed_bonus += nspeed;
}
void Creature::mod_dodge_bonus(int ndodge)
{
    dodge_bonus += ndodge;
}
void Creature::mod_block_bonus(int nblock)
{
    block_bonus += nblock;
}
void Creature::mod_hit_bonus(int nhit)
{
    hit_bonus += nhit;
}
void Creature::mod_bash_bonus(int nbash)
{
    bash_bonus += nbash;
}
void Creature::mod_cut_bonus(int ncut)
{
    cut_bonus += ncut;
}

void Creature::set_bash_mult(float nbashmult)
{
    bash_mult = nbashmult;
}
void Creature::set_cut_mult(float ncutmult)
{
    cut_mult = ncutmult;
}

void Creature::set_melee_quiet(bool nquiet)
{
    melee_quiet = nquiet;
}
void Creature::set_grab_resist(int ngrabres)
{
    grab_resist = ngrabres;
}
void Creature::set_throw_resist(int nthrowres)
{
    throw_resist = nthrowres;
}

int Creature::weight_capacity() const
{
    int base_carry = 13000;
    switch( get_size() ) {
    case MS_TINY:
        base_carry /= 4;
        break;
    case MS_SMALL:
        base_carry /= 2;
        break;
    case MS_MEDIUM:
    default:
        break;
    case MS_LARGE:
        base_carry *= 2;
        break;
    case MS_HUGE:
        base_carry *= 4;
        break;
    }

    return base_carry;
}

int Creature::get_weight() const
{
    switch( get_size() ) {
        case MS_TINY:
            return 1000;
        case MS_SMALL:
            return 40750;
        case MS_MEDIUM:
            return 81500;
        case MS_LARGE:
            return 120000;
        case MS_HUGE:
            return 200000;
    }

    return 0;
}

/*
 * Drawing-related functions
 */
void Creature::draw(WINDOW *w, int player_x, int player_y, bool inverted) const
{
    draw( w, tripoint( player_x, player_y, posz() ), inverted );
}

void Creature::draw( WINDOW *w, const tripoint &p, bool inverted ) const
{
    int draw_x = getmaxx(w) / 2 + posx() - p.x;
    int draw_y = getmaxy(w) / 2 + posy() - p.y;
    if(inverted) {
        mvwputch_inv(w, draw_y, draw_x, basic_symbol_color(), symbol());
    } else if(is_symbol_highlighted()) {
        mvwputch_hi(w, draw_y, draw_x, basic_symbol_color(), symbol());
    } else {
        mvwputch(w, draw_y, draw_x, symbol_color(), symbol() );
    }
}

bool Creature::is_symbol_highlighted() const
{
    return false;
}

body_part Creature::select_body_part(Creature *source, int hit_roll) const
{
    // Get size difference (-1,0,1);
    int szdif = source->get_size() - get_size();
    if(szdif < -1) {
        szdif = -1;
    } else if (szdif > 1) {
        szdif = 1;
    }

    add_msg( m_debug, "hit roll = %d", hit_roll);
    add_msg( m_debug, "source size = %d", source->get_size() );
    add_msg( m_debug, "target size = %d", get_size() );
    add_msg( m_debug, "difference = %d", szdif );

    std::map<body_part, double> hit_weights = default_hit_weights[szdif];

    // If the target is on the ground, even small/tiny creatures may target eyes/head. Also increases chances of larger creatures.
    // Any hit modifiers to locations should go here. (Tags, attack style, etc)
    if(is_on_ground()) {
        hit_weights[bp_eyes] += 1;
        hit_weights[bp_head] += 5;
    }

    //Adjust based on hit roll: Eyes, Head & Torso get higher, while Arms and Legs get lower.
    //This should eventually be replaced with targeted attacks and this being miss chances.
    // pow() is unstable at 0, so don't apply any changes.
    if( hit_roll != 0 ) {
        hit_weights[bp_eyes] *= std::pow(hit_roll, 1.15);
        hit_weights[bp_head] *= std::pow(hit_roll, 1.35);
        hit_weights[bp_torso] *= std::pow(hit_roll, 1);
        hit_weights[bp_arm_l] *= std::pow(hit_roll, 0.95);
        hit_weights[bp_arm_r] *= std::pow(hit_roll, 0.95);
        hit_weights[bp_leg_l] *= std::pow(hit_roll, 0.975);
        hit_weights[bp_leg_r] *= std::pow(hit_roll, 0.975);
    }

    // Debug for seeing weights.
    add_msg( m_debug, "eyes = %f", hit_weights.at( bp_eyes ) );
    add_msg( m_debug, "head = %f", hit_weights.at( bp_head ) );
    add_msg( m_debug, "torso = %f", hit_weights.at( bp_torso ) );
    add_msg( m_debug, "arm_l = %f", hit_weights.at( bp_arm_l ) );
    add_msg( m_debug, "arm_r = %f", hit_weights.at( bp_arm_r ) );
    add_msg( m_debug, "leg_l = %f", hit_weights.at( bp_leg_l ) );
    add_msg( m_debug, "leg_r = %f", hit_weights.at( bp_leg_r ) );

    double totalWeight = 0;
    for( const auto &hit_weight : hit_weights ) {
        totalWeight += hit_weight.second;
    }

    double roll = rng_float(0, totalWeight);
    body_part selected_part = bp_torso;

    for( const auto &hit_candidate : hit_weights) {
        roll -= hit_candidate.second;
        if(roll <= 0) {
            selected_part = hit_candidate.first;
            break;
        }
    }

    add_msg( m_debug, "selected part: %s", body_part_name(selected_part).c_str() );

    return selected_part;
}

bool Creature::compare_by_dist_to_point::operator()( const Creature* const a, const Creature* const b ) const
{
    return rl_dist( a->pos(), center ) < rl_dist( b->pos(), center );
}

void Creature::check_dead_state() {
    if( is_dead_state() ) {
        die( nullptr );
    }
}
