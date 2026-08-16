#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <graphx.h>
#include <keypadc.h>
#include <fileioc.h>
#include <tice.h>

/*
 * Slope CE Native
 * A from-scratch TI-84 Plus CE implementation of the Slope gameplay loop.
 *
 * Rendering is intentionally integer/fixed-point only. The calculator has no GPU,
 * so the road, obstacles, skyline, ball and lighting are generated from primitives.
 */

#define SCREEN_W 320
#define SCREEN_H 240
#define HORIZON_Y 66
#define FOCAL_X 92
#define FOCAL_Y 104
#define CAMERA_HEIGHT 38
#define NEAR_Z 18
#define BALL_Z 42
#define SEG_LEN 14
#define SEG_COUNT 30
#define Q 256

#define TRACK_HALF_W 28
#define BALL_RADIUS 4
#define MAX_LATERAL_V 820
#define START_SPEED 310
#define MAX_SPEED 740

#define SAVE_NAME "SLOPEHS"
#define SAVE_MAGIC 0x534C5045UL

enum {
    C_BLACK = 0,
    C_BG_GREEN,
    C_GRID_DIM,
    C_TRACK_DARK,
    C_TRACK,
    C_TRACK_BRIGHT,
    C_RED_DARK,
    C_RED,
    C_RED_BRIGHT,
    C_BALL_DARK,
    C_BALL,
    C_BALL_BRIGHT,
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
    int8_t obstacle_lane;
    uint8_t obstacle_size;
    uint8_t left_tower;
    uint8_t right_tower;
} Segment;

typedef struct {
    int x;
    int y;
    bool visible;
} ScreenPoint;

typedef struct {
    uint32_t magic;
    uint32_t high_score;
} SaveData;

static Segment segments[SEG_COUNT];
static uint32_t rng_state = 0x9E3779B9UL;
static int16_t generation_curve = 0;
static int16_t generation_height_velocity = 0;
static uint8_t generation_cooldown = 0;

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
    gfx_palette[C_BG_GREEN]     = gfx_RGBTo1555(0, 32, 20);
    gfx_palette[C_GRID_DIM]     = gfx_RGBTo1555(0, 74, 43);
    gfx_palette[C_TRACK_DARK]   = gfx_RGBTo1555(0, 92, 45);
    gfx_palette[C_TRACK]        = gfx_RGBTo1555(0, 178, 82);
    gfx_palette[C_TRACK_BRIGHT] = gfx_RGBTo1555(52, 255, 136);
    gfx_palette[C_RED_DARK]     = gfx_RGBTo1555(96, 0, 10);
    gfx_palette[C_RED]          = gfx_RGBTo1555(220, 16, 38);
    gfx_palette[C_RED_BRIGHT]   = gfx_RGBTo1555(255, 92, 102);
    gfx_palette[C_BALL_DARK]    = gfx_RGBTo1555(0, 92, 45);
    gfx_palette[C_BALL]         = gfx_RGBTo1555(34, 230, 116);
    gfx_palette[C_BALL_BRIGHT]  = gfx_RGBTo1555(166, 255, 204);
    gfx_palette[C_WHITE]        = gfx_RGBTo1555(255, 255, 255);
    gfx_palette[C_GREY]         = gfx_RGBTo1555(118, 135, 127);
    gfx_palette[C_SHADOW]       = gfx_RGBTo1555(0, 30, 18);
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

static void generate_segment(Segment *out, const Segment *prev, uint8_t index_from_start) {
    uint32_t r = rand32();
    int16_t cx = prev->center_x;
    int16_t h = prev->height;

    if ((r & 31U) == 0U) generation_curve += (int16_t)(((r >> 8) & 1U) ? 2 : -2);
    generation_curve = clamp16(generation_curve, -4, 4);
    if ((r & 127U) == 3U) generation_curve = 0;
    cx = clamp16((int16_t)(cx + generation_curve), -54, 54);

    if (((r >> 4) & 63U) == 7U && generation_height_velocity == 0) {
        generation_height_velocity = (int16_t)(((r >> 15) & 1U) ? 2 : -2);
    }
    if (generation_height_velocity != 0) {
        h = (int16_t)(h + generation_height_velocity);
        if (h > 18 || h < -12 || ((r >> 10) & 7U) == 0U) generation_height_velocity = 0;
    }

    out->center_x = cx;
    out->height = h;
    out->half_width = TRACK_HALF_W;
    out->solid = 1;
    out->ramp = 0;
    out->obstacle_lane = -2;
    out->obstacle_size = 9;
    out->left_tower = (uint8_t)(8 + ((r >> 16) & 31U));
    out->right_tower = (uint8_t)(8 + ((r >> 21) & 31U));

    if (index_from_start > 8 && generation_cooldown == 0) {
        uint8_t event = (uint8_t)((r >> 24) & 31U);
        if (event <= 5) {
            out->obstacle_lane = (int8_t)((r % 3U) - 1);
            out->obstacle_size = (uint8_t)(8 + ((r >> 5) & 3U));
            generation_cooldown = 2;
        } else if (event == 6) {
            out->half_width = 20;
            generation_cooldown = 2;
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

    segments[0].center_x = 0;
    segments[0].height = 0;
    segments[0].half_width = TRACK_HALF_W;
    segments[0].solid = 1;
    segments[0].obstacle_lane = -2;
    segments[0].left_tower = 12;
    segments[0].right_tower = 16;

    for (i = 1; i < SEG_COUNT; ++i) generate_segment(&segments[i], &segments[i - 1], i);

    segments[10].ramp = 1;
    segments[11].solid = 0;
    segments[12].solid = 0;
    segments[13].solid = 1;
    segments[11].obstacle_lane = -2;
    segments[12].obstacle_lane = -2;
}

static void append_far_segment(void) {
    uint8_t i;
    Segment old_last = segments[SEG_COUNT - 1];
    for (i = 0; i < SEG_COUNT - 1; ++i) segments[i] = segments[i + 1];
    generate_segment(&segments[SEG_COUNT - 1], &old_last, SEG_COUNT - 1);

    if ((rand32() & 63U) == 7U && segments[SEG_COUNT - 5].solid && segments[SEG_COUNT - 4].solid) {
        segments[SEG_COUNT - 5].ramp = 1;
        segments[SEG_COUNT - 4].solid = 0;
        segments[SEG_COUNT - 3].solid = 0;
        segments[SEG_COUNT - 4].obstacle_lane = -2;
        segments[SEG_COUNT - 3].obstacle_lane = -2;
        segments[SEG_COUNT - 2].solid = 1;
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

static ScreenPoint project_point(int16_t world_x, int16_t world_y, int16_t z) {
    ScreenPoint p;
    int32_t rel_x;
    if (z < 5) {
        p.x = p.y = 0;
        p.visible = false;
        return p;
    }
    rel_x = ((int32_t)world_x * Q) - camera_x_q8;
    p.x = (SCREEN_W / 2) + (int)(rel_x * FOCAL_X / ((int32_t)z * Q));
    p.y = HORIZON_Y + (int)(((int32_t)(CAMERA_HEIGHT - world_y) * FOCAL_Y) / z);
    p.visible = p.x > -80 && p.x < SCREEN_W + 80 && p.y > -80 && p.y < SCREEN_H + 80;
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

static void sample_track_at_ball(int16_t *center, int16_t *height, int16_t *half_width, bool *solid, bool *ramp) {
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
        int16_t z, obstacle_x, dx, ball_bottom;
        if (!s->solid || s->obstacle_lane < -1) continue;
        z = segment_z(i);
        if (z < BALL_Z - 5 || z > BALL_Z + 5) continue;
        obstacle_x = (int16_t)(s->center_x + s->obstacle_lane * 13);
        dx = (int16_t)((player_x_q8 >> 8) - obstacle_x);
        if (dx < 0) dx = (int16_t)-dx;
        ball_bottom = (int16_t)((ball_y_q8 >> 8) - BALL_RADIUS);
        if (dx <= (int16_t)(s->obstacle_size / 2 + BALL_RADIUS) &&
            ball_bottom < (int16_t)(s->height + s->obstacle_size)) {
            die();
            return;
        }
    }
}

static void update_game(bool left, bool right) {
    int16_t center, ground_h, half_w, px, ball_bottom;
    bool solid, ramp;

    if (left && !right) player_vx_q8 -= 58;
    if (right && !left) player_vx_q8 += 58;
    player_vx_q8 = (player_vx_q8 * 232) >> 8;
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
    speed_q8 = (uint16_t)(START_SPEED + (score > 215 ? 430 : score * 2));
    if (speed_q8 > MAX_SPEED) speed_q8 = MAX_SPEED;
    roll_phase = (uint8_t)(roll_phase + (speed_q8 >> 7));

    sample_track_at_ball(&center, &ground_h, &half_w, &solid, &ramp);
    px = (int16_t)(player_x_q8 >> 8);

    if (grounded) {
        if (!solid || px < center - half_w + BALL_RADIUS || px > center + half_w - BALL_RADIUS) {
            grounded = false;
            ball_vy_q8 = 0;
        } else {
            ball_y_q8 = (int32_t)(ground_h + BALL_RADIUS) * Q;
            if (ramp && !ramp_consumed) {
                ball_vy_q8 = 980;
                grounded = false;
                ramp_consumed = true;
            }
        }
    }

    if (!grounded) {
        ball_vy_q8 -= 74;
        ball_y_q8 += ball_vy_q8;
        ball_bottom = (int16_t)((ball_y_q8 >> 8) - BALL_RADIUS);
        if (solid && ball_vy_q8 <= 0 &&
            px >= center - half_w + BALL_RADIUS && px <= center + half_w - BALL_RADIUS &&
            ball_bottom <= ground_h + 2 && ball_bottom >= ground_h - 6) {
            grounded = true;
            ball_vy_q8 = 0;
            ball_y_q8 = (int32_t)(ground_h + BALL_RADIUS) * Q;
        }
    }

    if ((ball_y_q8 >> 8) < -28) {
        die();
        return;
    }
    check_obstacle_collision();
}

static void fill_quad(ScreenPoint a, ScreenPoint b, ScreenPoint c, ScreenPoint d, uint8_t color) {
    gfx_SetColor(color);
    gfx_FillTriangle(a.x, a.y, b.x, b.y, c.x, c.y);
    gfx_FillTriangle(a.x, a.y, c.x, c.y, d.x, d.y);
}

static void draw_background(void) {
    uint8_t i;
    gfx_FillScreen(C_BLACK);
    gfx_SetColor(C_BG_GREEN);
    gfx_HorizLine(0, HORIZON_Y, SCREEN_W);
    gfx_SetColor(C_GRID_DIM);
    for (i = 0; i < 9; ++i) {
        int x = 20 + i * 35;
        gfx_Line(SCREEN_W / 2, HORIZON_Y, x, SCREEN_H - 1);
    }
    for (i = 0; i < 5; ++i) {
        int y = HORIZON_Y + 12 + i * i * 7;
        if (y < SCREEN_H) gfx_HorizLine(0, y, SCREEN_W);
    }
}

static void draw_tower(const Segment *s, int16_t z, bool right_side) {
    int16_t base_x = (int16_t)(s->center_x + (right_side ? s->half_width + 42 : -s->half_width - 42));
    int16_t h = right_side ? s->right_tower : s->left_tower;
    int16_t w = 12, depth = 6;
    ScreenPoint bl, br, tl, tr, bl2, br2, tl2, tr2;
    if (z < 12) return;
    bl = project_point((int16_t)(base_x - w), s->height, z);
    br = project_point((int16_t)(base_x + w), s->height, z);
    tl = project_point((int16_t)(base_x - w), (int16_t)(s->height + h), z);
    tr = project_point((int16_t)(base_x + w), (int16_t)(s->height + h), z);
    bl2 = project_point((int16_t)(base_x - w), s->height, (int16_t)(z + depth));
    br2 = project_point((int16_t)(base_x + w), s->height, (int16_t)(z + depth));
    tl2 = project_point((int16_t)(base_x - w), (int16_t)(s->height + h), (int16_t)(z + depth));
    tr2 = project_point((int16_t)(base_x + w), (int16_t)(s->height + h), (int16_t)(z + depth));
    if (!bl.visible && !br.visible && !tl.visible && !tr.visible) return;

    fill_quad(tl, tr, br, bl, C_BG_GREEN);
    fill_quad(tl2, tr2, tr, tl, C_TRACK_DARK);
    gfx_SetColor(C_GRID_DIM);
    gfx_Line(tl.x, tl.y, tr.x, tr.y);
    gfx_Line(tr.x, tr.y, br.x, br.y);
    gfx_Line(bl.x, bl.y, tl.x, tl.y);
    gfx_Line(tl2.x, tl2.y, tl.x, tl.y);
    gfx_Line(tr2.x, tr2.y, tr.x, tr.y);
    gfx_Line(bl2.x, bl2.y, bl.x, bl.y);
    gfx_Line(br2.x, br2.y, br.x, br.y);
}

static void draw_track(void) {
    int i;
    for (i = SEG_COUNT - 1; i >= 1; --i) {
        int16_t z = segment_z((uint8_t)i);
        if (z > 10) {
            draw_tower(&segments[i], z, false);
            draw_tower(&segments[i], z, true);
        }
    }

    for (i = SEG_COUNT - 2; i >= 0; --i) {
        const Segment *a = &segments[i];
        const Segment *b = &segments[i + 1];
        int16_t z0 = segment_z((uint8_t)i);
        int16_t z1 = segment_z((uint8_t)(i + 1));
        ScreenPoint l0, r0, l1, r1;
        uint8_t base;
        if (!a->solid || z1 < 7 || z0 > 460) continue;
        if (z0 < 7) z0 = 7;

        l0 = project_point((int16_t)(a->center_x - a->half_width), a->height, z0);
        r0 = project_point((int16_t)(a->center_x + a->half_width), a->height, z0);
        l1 = project_point((int16_t)(b->center_x - b->half_width), b->height, z1);
        r1 = project_point((int16_t)(b->center_x + b->half_width), b->height, z1);
        if (!l0.visible && !r0.visible && !l1.visible && !r1.visible) continue;

        base = ((i + (scroll_q8 >> 9)) & 1) ? C_TRACK_DARK : C_TRACK;
        fill_quad(l0, r0, r1, l1, base);
        gfx_SetColor(C_TRACK_BRIGHT);
        gfx_Line(l0.x, l0.y, l1.x, l1.y);
        gfx_Line(r0.x, r0.y, r1.x, r1.y);
        gfx_SetColor(C_GRID_DIM);
        gfx_Line(l1.x, l1.y, r1.x, r1.y);

        if ((i & 1) == 0) {
            ScreenPoint m0 = project_point(a->center_x, (int16_t)(a->height + 1), z0);
            ScreenPoint m1 = project_point(b->center_x, (int16_t)(b->height + 1), z1);
            gfx_SetColor(C_TRACK_BRIGHT);
            gfx_Line(m0.x, m0.y, m1.x, m1.y);
        }
    }
}

static void draw_box(const Segment *s, int16_t z) {
    int16_t cx = (int16_t)(s->center_x + s->obstacle_lane * 13);
    int16_t hw = (int16_t)(s->obstacle_size / 2);
    int16_t h = s->obstacle_size;
    int16_t d = (int16_t)(s->obstacle_size / 2 + 2);
    ScreenPoint fbl, fbr, ftl, ftr, bbr, btl, btr;
    if (z < 8) return;
    fbl = project_point((int16_t)(cx - hw), s->height, (int16_t)(z - d));
    fbr = project_point((int16_t)(cx + hw), s->height, (int16_t)(z - d));
    ftl = project_point((int16_t)(cx - hw), (int16_t)(s->height + h), (int16_t)(z - d));
    ftr = project_point((int16_t)(cx + hw), (int16_t)(s->height + h), (int16_t)(z - d));
    bbr = project_point((int16_t)(cx + hw), s->height, (int16_t)(z + d));
    btl = project_point((int16_t)(cx - hw), (int16_t)(s->height + h), (int16_t)(z + d));
    btr = project_point((int16_t)(cx + hw), (int16_t)(s->height + h), (int16_t)(z + d));

    fill_quad(ftl, ftr, fbr, fbl, C_RED);
    fill_quad(btl, btr, ftr, ftl, C_RED_BRIGHT);
    fill_quad(ftr, btr, bbr, fbr, C_RED_DARK);
    gfx_SetColor(C_RED_BRIGHT);
    gfx_Line(ftl.x, ftl.y, ftr.x, ftr.y);
    gfx_Line(ftl.x, ftl.y, fbl.x, fbl.y);
}

static void draw_obstacles(void) {
    int i;
    for (i = SEG_COUNT - 1; i >= 0; --i) {
        if (segments[i].solid && segments[i].obstacle_lane >= -1) draw_box(&segments[i], segment_z((uint8_t)i));
    }
}

static void draw_ball(void) {
    int16_t world_x = (int16_t)(player_x_q8 >> 8);
    int16_t world_y = (int16_t)(ball_y_q8 >> 8);
    ScreenPoint p = project_point(world_x, world_y, BALL_Z);
    int radius = (BALL_RADIUS * FOCAL_X) / BALL_Z;
    int16_t center, ground_h, half_w;
    bool solid, ramp;
    ScreenPoint shadow;
    (void)center; (void)half_w; (void)solid; (void)ramp;
    if (radius < 5) radius = 5;
    if (radius > 16) radius = 16;

    sample_track_at_ball(&center, &ground_h, &half_w, &solid, &ramp);
    shadow = project_point(world_x, (int16_t)(ground_h + 1), BALL_Z);
    gfx_SetColor(C_SHADOW);
    gfx_FillEllipse(shadow.x, shadow.y + 2, (uint24_t)(radius + 3), (uint24_t)(radius / 2));
    gfx_SetColor(C_BALL_DARK);
    gfx_FillCircle(p.x, p.y, (uint24_t)(radius + 1));
    gfx_SetColor(C_BALL);
    gfx_FillCircle(p.x - 1, p.y - 1, (uint24_t)radius);
    gfx_SetColor(C_BALL_BRIGHT);
    gfx_FillCircle(p.x - radius / 3, p.y - radius / 3, (uint24_t)(radius / 3));
    gfx_SetColor(C_BALL_DARK);
    gfx_Line(p.x - radius + 2, p.y + ((roll_phase & 7) - 4),
             p.x + radius - 2, p.y - ((roll_phase & 7) - 4));
}

static void draw_hud(void) {
    gfx_SetTextFGColor(C_WHITE);
    gfx_SetTextScale(1, 1);
    gfx_PrintStringXY("SCORE", 6, 5);
    gfx_SetTextXY(54, 5);
    gfx_PrintUInt((unsigned int)score, 1);
    gfx_PrintStringXY("BEST", 236, 5);
    gfx_SetTextXY(276, 5);
    gfx_PrintUInt((unsigned int)high_score, 1);
}

static void draw_title(void) {
    draw_background();
    gfx_SetTextFGColor(C_TRACK_BRIGHT);
    gfx_SetTextScale(3, 3);
    gfx_PrintStringXY("SLOPE", 77, 58);
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(C_WHITE);
    gfx_PrintStringXY("TI-84 PLUS CE NATIVE", 83, 102);
    gfx_SetTextFGColor(C_TRACK_BRIGHT);
    gfx_PrintStringXY("LEFT / RIGHT TO STEER", 75, 142);
    gfx_SetTextFGColor(C_WHITE);
    gfx_PrintStringXY("[2nd] OR [enter] TO START", 58, 163);
    gfx_SetTextFGColor(C_GREY);
    gfx_PrintStringXY("[clear] quits", 112, 192);
}

static void draw_dead(void) {
    draw_background();
    draw_track();
    draw_obstacles();
    draw_ball();
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle(58, 63, 204, 116);
    gfx_SetColor(C_RED);
    gfx_Rectangle(58, 63, 204, 116);
    gfx_SetTextFGColor(C_RED_BRIGHT);
    gfx_SetTextScale(2, 2);
    gfx_PrintStringXY("GAME OVER", 82, 77);
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(C_WHITE);
    gfx_PrintStringXY("SCORE", 100, 116);
    gfx_SetTextXY(154, 116);
    gfx_PrintUInt((unsigned int)score, 1);
    gfx_PrintStringXY("BEST", 100, 135);
    gfx_SetTextXY(154, 135);
    gfx_PrintUInt((unsigned int)high_score, 1);
    gfx_SetTextFGColor(C_TRACK_BRIGHT);
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
    gfx_SetColor(C_TRACK_BRIGHT);
    gfx_Rectangle(92, 92, 136, 50);
    gfx_SetTextFGColor(C_WHITE);
    gfx_SetTextScale(2, 2);
    gfx_PrintStringXY("PAUSED", 108, 105);
    gfx_SetTextScale(1, 1);
}

static void render(void) {
    switch (mode) {
        case MODE_TITLE: draw_title(); break;
        case MODE_DEAD: draw_dead(); break;
        case MODE_PAUSE: draw_pause(); break;
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

        press_start = edge(second, &key_prev_2nd) | edge(enter, &key_prev_enter);
        press_pause = edge(alpha, &key_prev_alpha);
        press_clear = edge(clear, &key_prev_clear);

        if (press_clear) {
            if (mode == MODE_TITLE) running = false;
            else {
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
        delay(28);
    }

    gfx_End();
    kb_Reset();
    if (high_score > 0 || save_needed) save_high_score();
    return 0;
}
