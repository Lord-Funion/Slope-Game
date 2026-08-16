#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <graphx.h>
#include <keypadc.h>
#include <fileioc.h>
#include <tice.h>

/*
 * Slope CE Native -- performance pass
 *
 * Keep the recognizable Slope presentation, but design the renderer around
 * the eZ80 instead of around a desktop 3D pipeline.  The critical rule here
 * is that project_point() performs NO division during gameplay.  A reciprocal
 * table is built once at startup, then projection is multiply + shift.
 */

#define SCREEN_W 320
#define SCREEN_H 240
#define HORIZON_Y 54
#define FOCAL_X 102
#define FOCAL_Y 112
#define CAMERA_HEIGHT 39
#define NEAR_Z 16
#define BALL_Z 43
#define SEG_LEN 13
#define SEG_COUNT 32
#define RENDER_SEGMENTS 24
#define MAX_PROJECT_Z 430
#define RECIP_SHIFT 12
#define Q 256

#define TRACK_HALF_W 29
#define BALL_RADIUS 5
#define MAX_LATERAL_V 840
#define START_SPEED 300
#define MAX_SPEED 760
#define LANE_SPACING 14

#define SAVE_NAME "SLOPEHS"
#define SAVE_MAGIC 0x534C5045UL

enum {
    C_BLACK = 0,
    C_VOID_GREEN,
    C_GRID_DIM,
    C_GREEN,
    C_GREEN_BRIGHT,
    C_RED_DIM,
    C_RED,
    C_RED_BRIGHT,
    C_BALL_DARK,
    C_WHITE,
    C_GREY,
    C_SHADOW,
    C_COUNT
};

typedef enum {
    MODE_TITLE,
    MODE_PLAY,
    MODE_DEAD,
    MODE_PAUSE
} GameMode;

typedef struct {
    int16_t center_x;
    int16_t height;
    uint8_t half_width;
    uint8_t solid;
    uint8_t ramp;
    uint8_t obstacle_mask;
    uint8_t obstacle_size;
    uint8_t left_tower;
    uint8_t right_tower;
} Segment;

typedef struct {
    int x;
    int y;
    uint8_t visible;
} ScreenPoint;

typedef struct {
    ScreenPoint p[5];
    uint8_t valid;
} RoadProjection;

typedef struct {
    uint32_t magic;
    uint32_t high_score;
} SaveData;

static Segment segments[SEG_COUNT];
static RoadProjection road_cache[RENDER_SEGMENTS + 1];
static uint16_t reciprocal_z[MAX_PROJECT_Z + 1];

static uint32_t rng_state = 0x9E3779B9UL;
static int16_t generation_curve;
static int16_t generation_height_velocity;
static uint8_t generation_cooldown;
static uint8_t generation_narrow_timer;

static int32_t player_x_q8;
static int32_t player_vx_q8;
static int32_t ball_y_q8;
static int32_t ball_vy_q8;
static int32_t camera_x_q8;
static uint16_t scroll_q8;
static uint16_t speed_q8;
static uint32_t distance_q8;
static uint32_t score;
static uint32_t high_score;
static uint8_t roll_phase;
static bool grounded;
static bool ramp_consumed;
static GameMode mode;
static GameMode resume_mode;

static bool key_prev_2nd;
static bool key_prev_enter;
static bool key_prev_alpha;
static bool key_prev_clear;

static uint32_t rand32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int16_t clamp16(int16_t v, int16_t lo, int16_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void init_palette(void) {
    gfx_palette[C_BLACK]        = gfx_RGBTo1555(0, 0, 0);
    gfx_palette[C_VOID_GREEN]   = gfx_RGBTo1555(0, 18, 9);
    gfx_palette[C_GRID_DIM]     = gfx_RGBTo1555(0, 86, 35);
    gfx_palette[C_GREEN]        = gfx_RGBTo1555(0, 218, 70);
    gfx_palette[C_GREEN_BRIGHT] = gfx_RGBTo1555(73, 255, 113);
    gfx_palette[C_RED_DIM]      = gfx_RGBTo1555(74, 0, 8);
    gfx_palette[C_RED]          = gfx_RGBTo1555(216, 10, 28);
    gfx_palette[C_RED_BRIGHT]   = gfx_RGBTo1555(255, 54, 69);
    gfx_palette[C_BALL_DARK]    = gfx_RGBTo1555(0, 25, 10);
    gfx_palette[C_WHITE]        = gfx_RGBTo1555(245, 255, 248);
    gfx_palette[C_GREY]         = gfx_RGBTo1555(95, 112, 101);
    gfx_palette[C_SHADOW]       = gfx_RGBTo1555(0, 8, 3);
}

static void init_projection(void) {
    int z;
    memset(reciprocal_z, 0, sizeof(reciprocal_z));
    for (z = 5; z <= MAX_PROJECT_Z; ++z) {
        reciprocal_z[z] = (uint16_t)((1U << RECIP_SHIFT) / (unsigned)z);
    }
}

static void load_high_score(void) {
    SaveData data;
    uint8_t handle = ti_Open(SAVE_NAME, "r");
    high_score = 0;
    if (!handle) return;
    if (ti_Read(&data, sizeof(data), 1, handle) == 1 && data.magic == SAVE_MAGIC) {
        high_score = data.high_score;
    }
    ti_Close(handle);
}

static void save_high_score(void) {
    SaveData data;
    uint8_t handle;
    data.magic = SAVE_MAGIC;
    data.high_score = high_score;
    handle = ti_Open(SAVE_NAME, "w");
    if (!handle) return;
    if (ti_Write(&data, sizeof(data), 1, handle) == 1) {
        ti_SetArchiveStatus(true, handle);
    }
    ti_Close(handle);
}

static uint8_t random_obstacle_mask(uint32_t r) {
    switch ((uint8_t)((r >> 18) & 7U)) {
        case 0: return 0x01;
        case 1: return 0x02;
        case 2: return 0x04;
        case 3: return 0x03;
        case 4: return 0x06;
        case 5: return 0x05;
        default: return (uint8_t)(1U << (r % 3U));
    }
}

static void generate_segment(Segment *out, const Segment *prev, uint8_t index_from_start) {
    uint32_t r = rand32();
    int16_t cx = prev->center_x;
    int16_t h = prev->height;

    if ((r & 31U) == 0U) generation_curve += (int16_t)(((r >> 8) & 1U) ? 1 : -1);
    if ((r & 127U) == 17U) generation_curve = 0;
    generation_curve = clamp16(generation_curve, -4, 4);
    cx = clamp16((int16_t)(cx + generation_curve), -58, 58);

    if (((r >> 4) & 63U) == 7U && generation_height_velocity == 0) {
        generation_height_velocity = (int16_t)(((r >> 15) & 1U) ? 2 : -2);
    }
    if (generation_height_velocity != 0) {
        h = (int16_t)(h + generation_height_velocity);
        if (h > 20 || h < -14 || ((r >> 10) & 7U) == 0U) generation_height_velocity = 0;
    }

    out->center_x = cx;
    out->height = h;
    out->half_width = TRACK_HALF_W;
    out->solid = 1;
    out->ramp = 0;
    out->obstacle_mask = 0;
    out->obstacle_size = (uint8_t)(9 + ((r >> 5) & 3U));
    out->left_tower = (uint8_t)(18 + ((r >> 16) & 31U));
    out->right_tower = (uint8_t)(18 + ((r >> 21) & 31U));

    if (generation_narrow_timer) {
        out->half_width = (uint8_t)(20 + (generation_narrow_timer & 1U) * 2U);
        generation_narrow_timer--;
    }

    if (index_from_start > 7 && generation_cooldown == 0) {
        uint8_t event = (uint8_t)((r >> 24) & 31U);
        if (event <= 8) {
            out->obstacle_mask = random_obstacle_mask(r);
            generation_cooldown = (uint8_t)(2 + ((r >> 11) & 1U));
        } else if (event == 9) {
            generation_narrow_timer = 4;
            out->half_width = 20;
            generation_cooldown = 4;
        }
    }
    if (generation_cooldown) generation_cooldown--;
}

static void build_initial_track(void) {
    uint8_t i;
    memset(segments, 0, sizeof(segments));
    generation_curve = 0;
    generation_height_velocity = 0;
    generation_cooldown = 0;
    generation_narrow_timer = 0;

    segments[0].center_x = 0;
    segments[0].height = 0;
    segments[0].half_width = TRACK_HALF_W;
    segments[0].solid = 1;
    segments[0].left_tower = 28;
    segments[0].right_tower = 34;

    for (i = 1; i < SEG_COUNT; ++i) generate_segment(&segments[i], &segments[i - 1], i);

    segments[11].ramp = 1;
    segments[12].solid = 0;
    segments[13].solid = 0;
    segments[14].solid = 1;
    segments[12].obstacle_mask = 0;
    segments[13].obstacle_mask = 0;
}

static void append_far_segment(void) {
    uint8_t i;
    Segment old_last = segments[SEG_COUNT - 1];
    for (i = 0; i < SEG_COUNT - 1; ++i) segments[i] = segments[i + 1];
    generate_segment(&segments[SEG_COUNT - 1], &old_last, SEG_COUNT - 1);

    if ((rand32() & 79U) == 9U &&
        segments[SEG_COUNT - 6].solid && segments[SEG_COUNT - 5].solid &&
        segments[SEG_COUNT - 4].solid) {
        segments[SEG_COUNT - 6].ramp = 1;
        segments[SEG_COUNT - 5].solid = 0;
        segments[SEG_COUNT - 4].solid = 0;
        segments[SEG_COUNT - 5].obstacle_mask = 0;
        segments[SEG_COUNT - 4].obstacle_mask = 0;
        segments[SEG_COUNT - 3].solid = 1;
        segments[SEG_COUNT - 3].obstacle_mask = 0;
    }
}

static void reset_game(void) {
    rng_state ^= high_score + score + 0xA341316CUL;
    build_initial_track();
    player_x_q8 = 0;
    player_vx_q8 = 0;
    ball_y_q8 = BALL_RADIUS * Q;
    ball_vy_q8 = 0;
    camera_x_q8 = 0;
    scroll_q8 = 0;
    speed_q8 = START_SPEED;
    distance_q8 = 0;
    score = 0;
    roll_phase = 0;
    grounded = true;
    ramp_consumed = false;
    mode = MODE_PLAY;
}

/* Hot path: no divide. Camera sub-pixel precision is intentionally discarded. */
static ScreenPoint project_point(int16_t world_x, int16_t world_y, int16_t z) {
    ScreenPoint p;
    int16_t dx;
    uint16_t inv;

    if (z < 5 || z > MAX_PROJECT_Z) {
        p.x = 0;
        p.y = 0;
        p.visible = 0;
        return p;
    }

    inv = reciprocal_z[z];
    dx = (int16_t)(world_x - (int16_t)(camera_x_q8 >> 8));
    p.x = (SCREEN_W / 2) + (int)(((int32_t)dx * FOCAL_X * inv) >> RECIP_SHIFT);
    p.y = HORIZON_Y + (int)(((int32_t)(CAMERA_HEIGHT - world_y) * FOCAL_Y * inv) >> RECIP_SHIFT);
    p.visible = (uint8_t)(p.x > -48 && p.x < SCREEN_W + 48 && p.y > -48 && p.y < SCREEN_H + 48);
    return p;
}

static int16_t segment_z(uint8_t i) {
    return (int16_t)(NEAR_Z + (int16_t)i * SEG_LEN - (scroll_q8 >> 8));
}

static uint8_t ball_segment_index(uint8_t *frac_out) {
    int16_t local = (int16_t)(BALL_Z - NEAR_Z + (scroll_q8 >> 8));
    uint8_t idx;
    if (local < 0) local = 0;
    idx = (uint8_t)(local / SEG_LEN);
    if (idx >= SEG_COUNT - 1) idx = SEG_COUNT - 2;
    *frac_out = (uint8_t)(((local % SEG_LEN) * 255) / SEG_LEN);
    return idx;
}

static void sample_track_at_ball(int16_t *center, int16_t *height, int16_t *half_width,
                                 bool *solid, bool *ramp) {
    uint8_t frac;
    uint8_t idx = ball_segment_index(&frac);
    const Segment *a = &segments[idx];
    const Segment *b = &segments[idx + 1];
    *center = (int16_t)(a->center_x + (((int32_t)(b->center_x - a->center_x) * frac) >> 8));
    *height = (int16_t)(a->height + (((int32_t)(b->height - a->height) * frac) >> 8));
    *half_width = (int16_t)(a->half_width + (((int32_t)(b->half_width - a->half_width) * frac) >> 8));
    *solid = a->solid != 0;
    *ramp = a->ramp != 0;
}

static void die(void) {
    mode = MODE_DEAD;
    if (score > high_score) high_score = score;
}

static void check_obstacle_collision(void) {
    uint8_t i;
    for (i = 0; i < SEG_COUNT; ++i) {
        const Segment *s = &segments[i];
        int16_t z;
        uint8_t lane;
        if (!s->solid || s->obstacle_mask == 0) continue;
        z = segment_z(i);
        if (z < BALL_Z - 6 || z > BALL_Z + 6) continue;

        for (lane = 0; lane < 3; ++lane) {
            int16_t obstacle_x, dx, ball_bottom;
            if ((s->obstacle_mask & (1U << lane)) == 0) continue;
            obstacle_x = (int16_t)(s->center_x + ((int16_t)lane - 1) * LANE_SPACING);
            dx = (int16_t)((player_x_q8 >> 8) - obstacle_x);
            if (dx < 0) dx = (int16_t)-dx;
            ball_bottom = (int16_t)((ball_y_q8 >> 8) - BALL_RADIUS);
            if (dx <= (int16_t)(s->obstacle_size / 2 + BALL_RADIUS) &&
                ball_bottom < (int16_t)(s->height + s->obstacle_size + 2)) {
                die();
                return;
            }
        }
    }
}

static void update_game(bool left, bool right) {
    int16_t center, ground_h, half_w, px, ball_bottom;
    bool solid, ramp;

    if (left && !right) player_vx_q8 -= 55;
    if (right && !left) player_vx_q8 += 55;
    player_vx_q8 = (player_vx_q8 * 234) >> 8;
    player_vx_q8 = clamp32(player_vx_q8, -MAX_LATERAL_V, MAX_LATERAL_V);
    player_x_q8 += player_vx_q8;
    camera_x_q8 += (player_x_q8 - camera_x_q8) >> 4;

    scroll_q8 = (uint16_t)(scroll_q8 + speed_q8);
    distance_q8 += speed_q8;
    while (scroll_q8 >= SEG_LEN * Q) {
        scroll_q8 = (uint16_t)(scroll_q8 - SEG_LEN * Q);
        append_far_segment();
        ramp_consumed = false;
    }

    score = distance_q8 / (Q * 10UL);
    speed_q8 = (uint16_t)(START_SPEED + (score > 230 ? 460 : score * 2));
    if (speed_q8 > MAX_SPEED) speed_q8 = MAX_SPEED;
    roll_phase = (uint8_t)(roll_phase + 2 + (speed_q8 >> 8));

    sample_track_at_ball(&center, &ground_h, &half_w, &solid, &ramp);
    px = (int16_t)(player_x_q8 >> 8);

    if (grounded) {
        if (!solid || px < center - half_w + BALL_RADIUS || px > center + half_w - BALL_RADIUS) {
            grounded = false;
            ball_vy_q8 = 0;
        } else {
            ball_y_q8 = (int32_t)(ground_h + BALL_RADIUS) * Q;
            if (ramp && !ramp_consumed) {
                ball_vy_q8 = 1000;
                grounded = false;
                ramp_consumed = true;
            }
        }
    }

    if (!grounded) {
        ball_vy_q8 -= 75;
        ball_y_q8 += ball_vy_q8;
        ball_bottom = (int16_t)((ball_y_q8 >> 8) - BALL_RADIUS);
        if (solid && ball_vy_q8 <= 0 &&
            px >= center - half_w + BALL_RADIUS && px <= center + half_w - BALL_RADIUS &&
            ball_bottom <= ground_h + 2 && ball_bottom >= ground_h - 7) {
            grounded = true;
            ball_vy_q8 = 0;
            ball_y_q8 = (int32_t)(ground_h + BALL_RADIUS) * Q;
        }
    }

    if ((ball_y_q8 >> 8) < -32) {
        die();
        return;
    }
    check_obstacle_collision();
}

static bool on_screen(ScreenPoint p) {
    return p.x >= 0 && p.x < SCREEN_W && p.y >= 0 && p.y < SCREEN_H;
}

static void line_fast(ScreenPoint a, ScreenPoint b) {
    if (!a.visible && !b.visible) return;
    if (on_screen(a) && on_screen(b)) {
        gfx_Line_NoClip((uint24_t)a.x, (uint8_t)a.y, (uint24_t)b.x, (uint8_t)b.y);
    } else {
        gfx_Line(a.x, a.y, b.x, b.y);
    }
}

static void draw_background(void) {
    gfx_ZeroScreen();
    gfx_SetColor(C_VOID_GREEN);
    gfx_HorizLine_NoClip(0, HORIZON_Y, SCREEN_W);
}

static void build_road_cache(void) {
    uint8_t i;
    for (i = 0; i <= RENDER_SEGMENTS; ++i) {
        const Segment *s = &segments[i];
        int16_t z = segment_z(i);
        uint8_t lane;

        if (z < 7) z = 7;
        if (z > MAX_PROJECT_Z) {
            road_cache[i].valid = 0;
            continue;
        }
        road_cache[i].valid = 1;
        for (lane = 0; lane < 5; ++lane) {
            int16_t wx = (int16_t)(s->center_x - s->half_width +
                (((int16_t)(s->half_width * 2) * lane) >> 2));
            road_cache[i].p[lane] = project_point(wx, s->height, z);
        }
    }
}

static void draw_tower_front(const Segment *s, int16_t z, bool right_side) {
    int16_t base_x;
    int16_t h;
    ScreenPoint bl, br, tl, tr, ml, mr;

    if (z < 24 || z > 330) return;
    base_x = (int16_t)(s->center_x + (right_side ? s->half_width + 41 : -s->half_width - 41));
    h = right_side ? s->right_tower : s->left_tower;

    bl = project_point((int16_t)(base_x - 9), s->height, z);
    br = project_point((int16_t)(base_x + 9), s->height, z);
    tl = project_point((int16_t)(base_x - 9), (int16_t)(s->height + h), z);
    tr = project_point((int16_t)(base_x + 9), (int16_t)(s->height + h), z);
    if (!bl.visible && !br.visible && !tl.visible && !tr.visible) return;

    gfx_SetColor(C_GRID_DIM);
    line_fast(bl, tl);
    line_fast(tl, tr);
    line_fast(tr, br);

    ml = project_point((int16_t)(base_x - 9), (int16_t)(s->height + (h >> 1)), z);
    mr = project_point((int16_t)(base_x + 9), (int16_t)(s->height + (h >> 1)), z);
    line_fast(ml, mr);
}

static void draw_track(void) {
    int i;

    build_road_cache();

    /* Sparse city silhouettes: recognizable, but cheap enough for the eZ80. */
    for (i = RENDER_SEGMENTS - 2; i >= 3; i -= 4) {
        int16_t z = segment_z((uint8_t)i);
        draw_tower_front(&segments[i], z, false);
        draw_tower_front(&segments[i], z, true);
    }

    for (i = RENDER_SEGMENTS - 1; i >= 0; --i) {
        const RoadProjection *a = &road_cache[i];
        const RoadProjection *b = &road_cache[i + 1];
        uint8_t lane;
        if (!segments[i].solid || !a->valid || !b->valid) continue;

        /* Five longitudinal wires give the road its original neon-grid look. */
        for (lane = 0; lane < 5; ++lane) {
            gfx_SetColor(lane == 0 || lane == 4 || lane == 2 ? C_GREEN : C_GRID_DIM);
            line_fast(a->p[lane], b->p[lane]);
        }

        /* Crossbars only every other slab: about half the line work. */
        if ((i & 1) == 0) {
            gfx_SetColor(C_GRID_DIM);
            line_fast(b->p[0], b->p[4]);
        }

        if (segments[i].ramp) {
            gfx_SetColor(C_GREEN_BRIGHT);
            line_fast(a->p[0], a->p[4]);
        }
    }
}

static void draw_box_fast(const Segment *s, int16_t z, uint8_t lane) {
    int16_t cx, hw, h, d;
    ScreenPoint fbl, fbr, ftl, ftr, btl, btr;

    if (z < 12 || z > 330) return;
    cx = (int16_t)(s->center_x + ((int16_t)lane - 1) * LANE_SPACING);
    hw = (int16_t)(s->obstacle_size >> 1);
    h = (int16_t)(s->obstacle_size + 2);
    d = (int16_t)(s->obstacle_size / 2 + 3);

    fbl = project_point((int16_t)(cx - hw), s->height, (int16_t)(z - d));
    fbr = project_point((int16_t)(cx + hw), s->height, (int16_t)(z - d));
    ftl = project_point((int16_t)(cx - hw), (int16_t)(s->height + h), (int16_t)(z - d));
    ftr = project_point((int16_t)(cx + hw), (int16_t)(s->height + h), (int16_t)(z - d));
    btl = project_point((int16_t)(cx - hw), (int16_t)(s->height + h), (int16_t)(z + d));
    btr = project_point((int16_t)(cx + hw), (int16_t)(s->height + h), (int16_t)(z + d));

    gfx_SetColor(C_RED);
    line_fast(ftl, ftr);
    line_fast(ftr, fbr);
    line_fast(fbr, fbl);
    line_fast(fbl, ftl);
    gfx_SetColor(C_RED_DIM);
    line_fast(ftl, btl);
    line_fast(ftr, btr);
    line_fast(btl, btr);
}

static void draw_obstacles(void) {
    int i;
    for (i = RENDER_SEGMENTS - 1; i >= 0; --i) {
        uint8_t lane;
        if (!segments[i].solid || segments[i].obstacle_mask == 0) continue;
        for (lane = 0; lane < 3; ++lane) {
            if (segments[i].obstacle_mask & (1U << lane)) {
                draw_box_fast(&segments[i], segment_z((uint8_t)i), lane);
            }
        }
    }
}

static void draw_ball(void) {
    int16_t world_x = (int16_t)(player_x_q8 >> 8);
    int16_t world_y = (int16_t)(ball_y_q8 >> 8);
    ScreenPoint p = project_point(world_x, world_y, BALL_Z);
    int radius = 11;
    int phase = (int)((roll_phase >> 1) & 7U) - 3;
    int16_t center, ground_h, half_w;
    bool solid, ramp;
    ScreenPoint shadow;

    sample_track_at_ball(&center, &ground_h, &half_w, &solid, &ramp);
    shadow = project_point(world_x, (int16_t)(ground_h + 1), BALL_Z);

    /* Circle/straight chords are dramatically cheaper than multiple ellipses. */
    gfx_SetColor(C_SHADOW);
    gfx_FillCircle(shadow.x, shadow.y + 3, 7);
    gfx_SetColor(C_BALL_DARK);
    gfx_FillCircle(p.x, p.y, radius);
    gfx_SetColor(C_GREEN_BRIGHT);
    gfx_Circle(p.x, p.y, radius);
    gfx_SetColor(C_GREEN);
    gfx_Line(p.x - radius + 2, p.y + phase,
             p.x + radius - 2, p.y - phase);
    gfx_Line(p.x - radius + 3, p.y - 4,
             p.x + radius - 3, p.y - 4);
    gfx_Line(p.x - radius + 3, p.y + 4,
             p.x + radius - 3, p.y + 4);
    gfx_Line(p.x + phase, p.y - radius + 2,
             p.x - phase, p.y + radius - 2);
}

static unsigned score_digits(uint32_t v) {
    unsigned n = 1;
    while (v >= 10) {
        v /= 10;
        ++n;
    }
    return n;
}

static void draw_hud(void) {
    unsigned digits = score_digits(score);
    int x = (SCREEN_W - (int)(digits * 16U)) / 2;
    gfx_SetTextFGColor(C_GREEN_BRIGHT);
    gfx_SetTextScale(2, 2);
    gfx_SetTextXY(x, 6);
    gfx_PrintUInt((unsigned int)score, 1);
    gfx_SetTextScale(1, 1);
}

static void draw_title(void) {
    draw_background();
    gfx_SetTextFGColor(C_GREEN_BRIGHT);
    gfx_SetTextScale(4, 4);
    gfx_PrintStringXY("SLOPE", 60, 54);
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(C_WHITE);
    gfx_PrintStringXY("TI-84 PLUS CE", 106, 105);
    gfx_SetTextFGColor(C_GREEN);
    gfx_PrintStringXY("FAST NATIVE PORT", 92, 121);
    gfx_SetTextFGColor(C_WHITE);
    gfx_PrintStringXY("LEFT / RIGHT TO STEER", 75, 158);
    gfx_PrintStringXY("[2nd] OR [enter] TO START", 58, 178);
    gfx_SetTextFGColor(C_GREY);
    gfx_PrintStringXY("[clear] quits", 112, 207);
}

static void draw_dead(void) {
    draw_background();
    draw_track();
    draw_obstacles();
    draw_ball();
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle(66, 67, 188, 111);
    gfx_SetColor(C_RED);
    gfx_Rectangle(66, 67, 188, 111);
    gfx_SetTextFGColor(C_RED_BRIGHT);
    gfx_SetTextScale(2, 2);
    gfx_PrintStringXY("GAME OVER", 82, 79);
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(C_WHITE);
    gfx_PrintStringXY("SCORE", 99, 116);
    gfx_SetTextXY(155, 116);
    gfx_PrintUInt((unsigned int)score, 1);
    gfx_PrintStringXY("BEST", 99, 135);
    gfx_SetTextXY(155, 135);
    gfx_PrintUInt((unsigned int)high_score, 1);
    gfx_SetTextFGColor(C_GREEN_BRIGHT);
    gfx_PrintStringXY("[2nd] / [enter] AGAIN", 70, 160);
}

static void draw_pause(void) {
    draw_background();
    draw_track();
    draw_obstacles();
    draw_ball();
    draw_hud();
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle(92, 92, 136, 50);
    gfx_SetColor(C_GREEN_BRIGHT);
    gfx_Rectangle(92, 92, 136, 50);
    gfx_SetTextFGColor(C_WHITE);
    gfx_SetTextScale(2, 2);
    gfx_PrintStringXY("PAUSED", 108, 105);
    gfx_SetTextScale(1, 1);
}

static void render(void) {
    switch (mode) {
        case MODE_TITLE:
            draw_title();
            break;
        case MODE_DEAD:
            draw_dead();
            break;
        case MODE_PAUSE:
            draw_pause();
            break;
        case MODE_PLAY:
        default:
            draw_background();
            draw_track();
            draw_obstacles();
            draw_ball();
            draw_hud();
            break;
    }
    gfx_SwapDraw();
}

static bool edge(bool now, bool *prev) {
    bool pressed = now && !*prev;
    *prev = now;
    return pressed;
}

int main(void) {
    bool running = true;
    bool save_needed = false;

    load_high_score();
    init_projection();
    gfx_Begin();
    gfx_SetDrawBuffer();
    init_palette();
    gfx_SetTextTransparentColor(C_BLACK);
    mode = MODE_TITLE;
    resume_mode = MODE_PLAY;

    while (running) {
        bool left, right, second, enter, alpha, clear;
        bool press_start, press_pause, press_clear;

        kb_Scan();
        left = (kb_Data[7] & kb_Left) != 0;
        right = (kb_Data[7] & kb_Right) != 0;
        second = (kb_Data[1] & kb_2nd) != 0;
        enter = (kb_Data[6] & kb_Enter) != 0;
        alpha = (kb_Data[2] & kb_Alpha) != 0;
        clear = (kb_Data[6] & kb_Clear) != 0;

        press_start = edge(second, &key_prev_2nd);
        press_start = edge(enter, &key_prev_enter) || press_start;
        press_pause = edge(alpha, &key_prev_alpha);
        press_clear = edge(clear, &key_prev_clear);

        if (press_clear) {
            if (mode == MODE_TITLE) {
                running = false;
            } else {
                if (score > high_score) high_score = score;
                mode = MODE_TITLE;
                save_needed = true;
            }
        }

        if (press_start && (mode == MODE_TITLE || mode == MODE_DEAD)) reset_game();

        if (press_pause) {
            if (mode == MODE_PLAY) {
                resume_mode = mode;
                mode = MODE_PAUSE;
            } else if (mode == MODE_PAUSE) {
                mode = resume_mode;
            }
        }

        if (mode == MODE_PLAY) {
            GameMode before = mode;
            update_game(left, right);
            if (before == MODE_PLAY && mode == MODE_DEAD) save_needed = true;
        }

        render();
        /* Render cost now dominates pacing far less; keep gameplay near 40 Hz. */
        delay(20);
    }

    gfx_End();
    kb_Reset();
    if (high_score > 0 || save_needed) save_high_score();
    return 0;
}
