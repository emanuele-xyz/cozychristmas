#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <format>
#include <random>
#include <stacktrace>
#include <stacktrace>
#include <string>
#include <stdexcept>
#include <vector>

constexpr int SCREEN_W{720};
constexpr int SCREEN_H{720};
constexpr int MAP_SIDE{8};
constexpr int TILE_PIXEL_SIZE{14}; // TODO: find a better name
constexpr int SCORE_PIXEL_PAD{1};
constexpr int SCORE_PIXEL_HEIGHT{7};
constexpr int LOGICAL_SCREEN_W{MAP_SIDE * TILE_PIXEL_SIZE};
constexpr int LOGICAL_SCREEN_H{MAP_SIDE * TILE_PIXEL_SIZE + SCORE_PIXEL_PAD + SCORE_PIXEL_HEIGHT};
constexpr double SPAWN_TIME_SEC_START{2.0};
constexpr double SPAWN_TIME_DIFFICULTY_COEFFICIENT{0.01}; // decrease by % of the current spawn time
constexpr double MIN_SPAWN_TIME_SEC{0.5};
constexpr double SEC_PER_TICK_START{0.5};
constexpr double SEC_PER_TICK_DIFFICULTY_COEFFICIENT{0.001}; // decrease by % of the current sec per tick
constexpr double MIN_SEC_PER_TICK{0.375};

constexpr int MAX_SCORE{99};

#define error(msg) \
    throw Error { __FILE__, __LINE__, (msg) }

#define unreachable() error("unreachable code path")

class Error : public std::runtime_error
{
public:
    Error(const char *file, int line, const std::string &message)
        : std::runtime_error{std::format("{}({}): {}\n{}", file, line, message, std::to_string(std::stacktrace::current(1)))}
    {
    }
};

// used for seeding xoshiro256**
// ref: https://xorshift.di.unimi.it/splitmix64.c
class SplitMix64
{
public:
    SplitMix64(uint64_t seed) : x(seed) {}

    uint64_t next()
    {
        uint64_t z = (x += 0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }

private:
    uint64_t x;
};

// ref: https://prng.di.unimi.it/xoshiro256starstar.c
class Xoshiro256StarStar
{
public:
    Xoshiro256StarStar(uint64_t seed)
    {
        SplitMix64 splitmix{seed};
        s[0] = splitmix.next();
        s[1] = splitmix.next();
        s[2] = splitmix.next();
        s[3] = splitmix.next();
    }

    // default constructor, seed with current time
    Xoshiro256StarStar() : Xoshiro256StarStar(static_cast<uint64_t>(std::chrono::high_resolution_clock::now()
                                                                        .time_since_epoch()
                                                                        .count())) {}

    uint64_t next()
    {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;

        const uint64_t t = s[1] << 17;

        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];

        s[2] ^= t;

        s[3] = rotl(s[3], 45);

        return result;
    }

    // generate random integer in range [a, b] (inclusive)
    int64_t range(int64_t a, int64_t b)
    {
        assert(a <= b);
        uint64_t range = static_cast<uint64_t>(b - a) + 1;

        // use rejection sampling to avoid modulo bias
        // ref: https://github.com/openbsd/src/blob/master/lib/libc/crypt/arc4random_uniform.c
        uint64_t threshold = -range % range;
        uint64_t r;

        do
        {
            r = next();
        } while (r < threshold);

        return a + static_cast<int64_t>(r % range);
    }

private:
    uint64_t s[4];

    static inline uint64_t rotl(const uint64_t x, int k)
    {
        return (x << k) | (x >> (64 - k));
    }
};

enum TileType : uint8_t
{
    TILE_EMPTY,
    TILE_BAG,
    TILE_GIFT,
    TILE_HOUSE,
};

struct v2
{
    int row;
    int col;
};

struct Tile
{
    constexpr Tile(TileType tile_type, int previous_row = 0, int previous_column = 0) noexcept
        : type{tile_type}, prev_row{previous_row}, prev_col{previous_column}
    {
    }
    constexpr Tile() noexcept
        : Tile{TILE_EMPTY, -1, -1}
    {
    }

    TileType type;
    int prev_row; // Used only if type is TILE_BAG
    int prev_col; // Used only if type is TILE_BAG
};

enum Direction : uint8_t
{
    DIRECTION_NORTH,
    DIRECTION_SOUTH,
    DIRECTION_WEST,
    DIRECTION_EAST,
};

static int random_int(int lo, int hi) noexcept
{
    static Xoshiro256StarStar prng{}; // TODO - static or new seed each new game?
    return static_cast<int>(prng.range(lo, hi));
}

static int mod(int a, int b)
{
    return (a % b + b) % b;
}

struct SceneGameState
{
    Tile map[MAP_SIDE][MAP_SIDE]{};
    Direction prev_tick_direction; // Santa's direction at the previous tick
    Direction cur_tick_direction;  // Santa's direction at the current tick
    v2 santa;
    int num_bags;
    v2 first_bag;
    v2 last_bag;
    double spawn_time_sec{SPAWN_TIME_SEC_START};
    double spawn_timer{};
};

struct GameState
{
    SceneGameState scene;
    int score;
};

struct SoundEffects
{
    Mix_Chunk *start;
    Mix_Chunk *gift;
    Mix_Chunk *house;
    Mix_Chunk *hurt;
    Mix_Chunk *step;
    Mix_Chunk *spawn;
};

static bool update_game_state(GameState &state, const SoundEffects &sfx)
{
    bool game_over{false};

    // update previous tick direction
    state.scene.prev_tick_direction = state.scene.cur_tick_direction;

    // save old santa position for later
    v2 old_santa{state.scene.santa};

    // move santa
    switch (state.scene.cur_tick_direction)
    {
    case DIRECTION_NORTH:
    {
        state.scene.santa.row = mod(state.scene.santa.row - 1, MAP_SIDE);
    }
    break;
    case DIRECTION_SOUTH:
    {
        state.scene.santa.row = mod(state.scene.santa.row + 1, MAP_SIDE);
    }
    break;
    case DIRECTION_WEST:
    {
        state.scene.santa.col = mod(state.scene.santa.col - 1, MAP_SIDE);
    }
    break;
    case DIRECTION_EAST:
    {
        state.scene.santa.col = mod(state.scene.santa.col + 1, MAP_SIDE);
    }
    break;
    default:
    {
        unreachable();
    }
    }

    // run custom logic based on which tile santa is on
    switch (state.scene.map[state.scene.santa.row][state.scene.santa.col].type)
    {
    case TILE_EMPTY:
    {
        // if there are bags, employ logic to move them
        if (state.scene.num_bags > 0)
        {
            // make bag where santa was
            state.scene.map[old_santa.row][old_santa.col] = Tile{TILE_BAG};
            // save old first bag location
            v2 old_first_bag{state.scene.first_bag};
            // set new first bag location to where santa was
            state.scene.first_bag = old_santa;
            // update previous bag pointer for old fisrst bag
            state.scene.map[old_first_bag.row][old_first_bag.col] = Tile{TILE_BAG, old_santa.row, old_santa.col};
            // save previous to last bag position
            v2 previous_to_last_bag{state.scene.map[state.scene.last_bag.row][state.scene.last_bag.col].prev_row, state.scene.map[state.scene.last_bag.row][state.scene.last_bag.col].prev_col};
            // make empty tile where the last bag is
            state.scene.map[state.scene.last_bag.row][state.scene.last_bag.col] = Tile{TILE_EMPTY};
            // update last bag position to previos to last bag position
            state.scene.last_bag = previous_to_last_bag;
        }

        // play step sound effect
        Mix_PlayChannel(-1, sfx.step, 0);
    }
    break;
    case TILE_GIFT:
    {
        // make gift tile empty where santa is
        state.scene.map[state.scene.santa.row][state.scene.santa.col] = Tile{TILE_EMPTY};
        // spawn bag where santa was
        state.scene.map[old_santa.row][old_santa.col] = Tile{TILE_BAG};
        if (state.scene.num_bags <= 0)
        {
            // set first and last bag positions to where santa was
            state.scene.first_bag = old_santa;
            state.scene.last_bag = old_santa;
        }
        else // state.num_bags > 0
        {
            // update former first bag previous pointer with the new first bag
            state.scene.map[state.scene.first_bag.row][state.scene.first_bag.col] = Tile{TILE_BAG, old_santa.row, old_santa.col};
        }
        // update new first bag position
        state.scene.first_bag = old_santa;
        // increase number of bags
        state.scene.num_bags++;

        // play gift sound effect
        Mix_PlayChannel(-1, sfx.gift, 0);
    }
    break;
    case TILE_BAG:
    {
        game_over = true;
    }
    break;
    case TILE_HOUSE:
    {
        if (state.scene.num_bags <= 0)
        {
            game_over = true;

            // play hurt sound effect
            Mix_PlayChannel(-1, sfx.hurt, 0);
        }
        else // state.num_bags > 0
        {
            // make house tile empty where santa is
            state.scene.map[state.scene.santa.row][state.scene.santa.col] = Tile{TILE_EMPTY};
            // save last bag tile
            Tile saved = state.scene.map[state.scene.last_bag.row][state.scene.last_bag.col];
            // remove last bag tile
            state.scene.map[state.scene.last_bag.row][state.scene.last_bag.col] = Tile{TILE_EMPTY};
            if (state.scene.num_bags > 1)
            {
                // update last bag
                state.scene.last_bag = v2{saved.prev_row, saved.prev_col};
                // spawn bag where santa was
                state.scene.map[old_santa.row][old_santa.col] = Tile{TILE_BAG};
                // save former first bag
                v2 old_first_bag = state.scene.first_bag;
                // update new first bag position
                state.scene.first_bag = old_santa;
                // update former first bag previous pointer with the new first bag
                state.scene.map[old_first_bag.row][old_first_bag.col] = Tile{TILE_BAG, state.scene.first_bag.row, state.scene.first_bag.col};
                // save previous to last bag
                Tile last_bag{state.scene.map[state.scene.last_bag.row][state.scene.last_bag.col]};
                // remove last bag tile
                state.scene.map[state.scene.last_bag.row][state.scene.last_bag.col] = Tile{TILE_EMPTY};
                // update last bag position
                state.scene.last_bag = v2{last_bag.prev_row, last_bag.prev_col};
            }
            // decrease number of bags
            state.scene.num_bags--;
            // increase the number of successfully delivered gifts
            state.score++;
            // if the player reaches the maximum allowed score, the game is over
            game_over = (state.score == MAX_SCORE);

            // play house sound effect
            Mix_PlayChannel(-1, sfx.house, 0);
        }
    }
    break;
    default:
    {
        unreachable();
    }
    }

    // when spawn timer sets off, either spawn a gift or a house
    if (state.scene.spawn_timer >= state.scene.spawn_time_sec)
    {
        // spawning logic
        {
            std::vector<v2> empty_tiles{};
            for (int row{}; row < MAP_SIDE; row++)
            {
                for (int col{}; col < MAP_SIDE; col++)
                {
                    bool is_santa_on_tile{state.scene.santa.row == row && state.scene.santa.col == col};
                    if (state.scene.map[row][col].type == TILE_EMPTY && !is_santa_on_tile)
                    {
                        empty_tiles.emplace_back(row, col);
                    }
                }
            }

            if (empty_tiles.size() > 0)
            {
                // spawn either a gift or a house randomly
                int size{static_cast<int>(empty_tiles.size())};
                size_t idx{static_cast<size_t>(random_int(0, size - 1))};
                v2 random_tile{empty_tiles[idx]};
                state.scene.map[random_tile.row][random_tile.col] = Tile{random_int(1, 100) <= 50 ? TILE_GIFT : TILE_HOUSE};

                // play spawn sound
                Mix_PlayChannel(-1, sfx.spawn, 0);
            }
        }

        // make spawn time a little shorter (to make game harder)
        state.scene.spawn_time_sec -= SPAWN_TIME_DIFFICULTY_COEFFICIENT * state.scene.spawn_time_sec;
        // make sure spawn time doesn't go below the fixed minimum
        state.scene.spawn_time_sec = std::max(state.scene.spawn_time_sec, MIN_SPAWN_TIME_SEC);
        // reset timer
        state.scene.spawn_timer = 0.0;
    }

    return game_over;
}

class SDL2ExHandle
{
public:
    SDL2ExHandle()
    {
        int res{SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)};
        if (res < 0)
        {
            error(std::format("failed to initialize SDL2: {}", SDL_GetError()));
        }
    }
    ~SDL2ExHandle() noexcept
    {
        SDL_Quit();
    }
    SDL2ExHandle(const SDL2ExHandle &) noexcept = delete;
    SDL2ExHandle(SDL2ExHandle &&) noexcept = delete;
    SDL2ExHandle &operator=(const SDL2ExHandle &) noexcept = delete;
    SDL2ExHandle &operator=(SDL2ExHandle &&) noexcept = delete;
};

class SDL2ExImageHandle
{
public:
    SDL2ExImageHandle()
    {
        int result{IMG_Init(IMG_INIT_PNG)};
        if (!(result & IMG_INIT_PNG))
        {
            error(std::format("failed to initialize SDL2 image: {}", IMG_GetError()));
        }
    }
    ~SDL2ExImageHandle() noexcept
    {
        IMG_Quit();
    }
    SDL2ExImageHandle(const SDL2ExImageHandle &) noexcept = delete;
    SDL2ExImageHandle(SDL2ExImageHandle &&) noexcept = delete;
    SDL2ExImageHandle &operator=(const SDL2ExImageHandle &) noexcept = delete;
    SDL2ExImageHandle &operator=(SDL2ExImageHandle &&) noexcept = delete;
};

class SDL2ExMixerHandle
{
public:
    SDL2ExMixerHandle()
    {
        // Initialize SDL_mixer for Audio (44.1khz, default format, 2 channels, 2048 chunk size)
        int res{Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048)};
        if (res < 0)
        {
            error(std::format("failed to initialize SDL2 mixer: {}", Mix_GetError()));
        }
    }
    ~SDL2ExMixerHandle() noexcept
    {
        Mix_Quit();
    }
    SDL2ExMixerHandle(const SDL2ExMixerHandle &) noexcept = delete;
    SDL2ExMixerHandle(SDL2ExMixerHandle &&) noexcept = delete;
    SDL2ExMixerHandle &operator=(const SDL2ExMixerHandle &) noexcept = delete;
    SDL2ExMixerHandle &operator=(SDL2ExMixerHandle &&) noexcept = delete;
};

class SDL2ExWindow
{
public:
    SDL2ExWindow() : handle{}
    {
        handle = SDL_CreateWindow("Cozy Christmas",
                                  SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  SCREEN_W, SCREEN_H,
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
        if (!handle)
        {
            error(std::format("failed to create SDL2 window: {}", SDL_GetError()));
        }
    }
    ~SDL2ExWindow() noexcept
    {
        SDL_DestroyWindow(handle);
    }
    SDL2ExWindow(const SDL2ExWindow &) noexcept = delete;
    SDL2ExWindow(SDL2ExWindow &&) noexcept = delete;
    SDL2ExWindow &operator=(const SDL2ExWindow &) noexcept = delete;
    SDL2ExWindow &operator=(SDL2ExWindow &&) noexcept = delete;

public:
    constexpr SDL_Window *Handle() const noexcept { return handle; }

private:
    SDL_Window *handle;
};

class SDL2ExRenderer
{
public:
    SDL2ExRenderer(SDL_Window *window) : handle{}
    {
        // Create Renderer (Hardware accelerated and VSync enabled)
        handle = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!handle)
        {
            error(std::format("failed to create SDL2 renderer: {}", SDL_GetError()));
        }
    }
    ~SDL2ExRenderer() noexcept
    {
        SDL_DestroyRenderer(handle);
    }
    SDL2ExRenderer(const SDL2ExRenderer &) noexcept = delete;
    SDL2ExRenderer(SDL2ExRenderer &&) noexcept = delete;
    SDL2ExRenderer &operator=(const SDL2ExRenderer &) noexcept = delete;
    SDL2ExRenderer &operator=(SDL2ExRenderer &&) noexcept = delete;

public:
    constexpr SDL_Renderer *Handle() const noexcept { return handle; }

private:
    SDL_Renderer *handle;
};

class SDL2ExSurface
{
public:
    SDL2ExSurface(const char *file) : handle{}
    {
        handle = IMG_Load(file);
        if (!handle)
        {
            error(std::format("failed to create SDL2 surface for file '{}': {}", file, IMG_GetError()));
        }
    }
    ~SDL2ExSurface() noexcept
    {
        SDL_FreeSurface(handle);
    }
    SDL2ExSurface(const SDL2ExSurface &) noexcept = delete;
    SDL2ExSurface(SDL2ExSurface &&) noexcept = delete;
    SDL2ExSurface &operator=(const SDL2ExSurface &) noexcept = delete;
    SDL2ExSurface &operator=(SDL2ExSurface &&) noexcept = delete;

public:
    constexpr SDL_Surface *Handle() const noexcept { return handle; }

private:
    SDL_Surface *handle;
};

class SDL2ExTexture
{
public:
    SDL2ExTexture(const SDL2ExRenderer &renderer, const char *file) : handle{}
    {
        SDL2ExSurface tmp{file};
        handle = SDL_CreateTextureFromSurface(renderer.Handle(), tmp.Handle());
        if (!handle)
        {
            error(std::format("failed to create SDL2 texture for file '{}': {}", file, SDL_GetError()));
        }
    }
    ~SDL2ExTexture() noexcept
    {
        SDL_DestroyTexture(handle);
    }
    SDL2ExTexture(const SDL2ExTexture &) noexcept = delete;
    SDL2ExTexture(SDL2ExTexture &&) noexcept = delete;
    SDL2ExTexture &operator=(const SDL2ExTexture &) noexcept = delete;
    SDL2ExTexture &operator=(SDL2ExTexture &&) noexcept = delete;

public:
    constexpr SDL_Texture *Handle() const noexcept { return handle; }

private:
    SDL_Texture *handle;
};

class SDL2ExMusic
{
public:
    SDL2ExMusic(const char *file) : handle{}
    {
        handle = Mix_LoadMUS(file);
        if (!handle)
        {
            error(std::format("filed to load SDL2 music for file '{}': {}", file, Mix_GetError()));
        }
    }
    ~SDL2ExMusic() noexcept
    {
        Mix_FreeMusic(handle);
    }
    SDL2ExMusic(const SDL2ExMusic &) noexcept = delete;
    SDL2ExMusic(SDL2ExMusic &&) noexcept = delete;
    SDL2ExMusic &operator=(const SDL2ExMusic &) noexcept = delete;
    SDL2ExMusic &operator=(SDL2ExMusic &&) noexcept = delete;

public:
    constexpr Mix_Music *Handle() const noexcept { return handle; }

private:
    Mix_Music *handle;
};

class SDL2ExChunk
{
public:
    SDL2ExChunk(const char *file) : handle{}
    {
        handle = Mix_LoadWAV(file);
        if (!handle)
        {
            error(std::format("failed to load SDL2 chunk for file '{}': {}", file, Mix_GetError()));
        }
    }
    ~SDL2ExChunk() noexcept
    {
        Mix_FreeChunk(handle);
    }
    SDL2ExChunk(const SDL2ExChunk &) noexcept = delete;
    SDL2ExChunk(SDL2ExChunk &&) noexcept = delete;
    SDL2ExChunk &operator=(const SDL2ExChunk &) noexcept = delete;
    SDL2ExChunk &operator=(SDL2ExChunk &&) noexcept = delete;

public:
    constexpr Mix_Chunk *Handle() const noexcept { return handle; }

private:
    Mix_Chunk *handle;
};

class IScene
{
public:
    virtual void on_event(const SDL_Event &) {}
    virtual bool update(double dt_sec) = 0;
    virtual void render() = 0;

public:
    virtual ~IScene() noexcept = default;
};

class GameStartScene : public IScene
{
public:
    GameStartScene(GameState &game_state, SDL_Renderer *renderer, SDL_Texture *sprite_sheet, const SoundEffects &sfx) noexcept
        : m_game_state{game_state}, m_renderer{renderer}, m_sprite_sheet{sprite_sheet}, m_sfx{sfx}, m_game_over{false} {}
    ~GameStartScene() noexcept override = default;
    GameStartScene(const GameStartScene &) noexcept = delete;
    GameStartScene(GameStartScene &&) noexcept = delete;
    GameStartScene &operator=(const GameStartScene &) noexcept = delete;
    GameStartScene &operator=(GameStartScene &&) noexcept = delete;

public:
    void on_event(const SDL_Event &event) override
    {
        if (event.type == SDL_KEYDOWN)
        {
            // key presses events
            if (event.key.keysym.sym == SDLK_RETURN)
            {
                m_game_over = true;
                Mix_PlayChannel(-1, m_sfx.start, 0);
            }
        }
    }
    bool update(double /*dt*/) override
    {
        bool tmp{m_game_over};
        m_game_over = false;
        return tmp;
    }
    void render() override
    {
        // clear the screen to black
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
        SDL_RenderClear(m_renderer);

        // draw background
        {
            // set background to palette dark green (bounding boxes)
            SDL_SetRenderDrawColor(m_renderer, 31, 50, 36, 255);
            // define the area to draw
            SDL_Rect myRect{0, 0, LOGICAL_SCREEN_W, LOGICAL_SCREEN_H};
            // draw it
            SDL_RenderFillRect(m_renderer, &myRect);
        }

        // draw cozy christmas logo
        {
            constexpr int cozy_christmas_w{110};
            constexpr int cozy_christmas_h{44};

            SDL_Rect dst_rect{};
            dst_rect.x = (LOGICAL_SCREEN_W / 2) - (cozy_christmas_w / 2);
            dst_rect.y = (LOGICAL_SCREEN_H / 2) - (cozy_christmas_h / 2);
            dst_rect.w = cozy_christmas_w;
            dst_rect.h = cozy_christmas_h;

            SDL_Rect src_rect{};
            src_rect.x = 8;
            src_rect.y = 33;
            src_rect.w = cozy_christmas_w;
            src_rect.h = cozy_christmas_h;

            SDL_RenderCopy(m_renderer, m_sprite_sheet, &src_rect, &dst_rect);
        }
    }

protected:
    GameState &m_game_state;
    SDL_Renderer *m_renderer;
    SDL_Texture *m_sprite_sheet;
    const SoundEffects &m_sfx;
    bool m_game_over;
};

class EndGameScene : public GameStartScene
{
public:
    EndGameScene(GameState &game_state, SDL_Renderer *renderer, SDL_Texture *sprite_sheet, const SoundEffects &sfx) noexcept
        : GameStartScene{game_state, renderer, sprite_sheet, sfx} {}
    ~EndGameScene() noexcept override = default;
    EndGameScene(const EndGameScene &) noexcept = delete;
    EndGameScene(EndGameScene &&) noexcept = delete;
    EndGameScene &operator=(const EndGameScene &) noexcept = delete;
    EndGameScene &operator=(EndGameScene &&) noexcept = delete;

public:
    void render() override
    {
        GameStartScene::render();

        // either draw you won or you lost
        {
            SDL_Rect src_rect{};
            SDL_Rect dst_rect{};

            if (m_game_state.score == MAX_SCORE) // won
            {
                src_rect.x = 37;
                src_rect.y = 96;
                src_rect.w = 84 - 37;
                src_rect.h = 105 - 96;
            }
            else // lost
            {
                src_rect.x = 32;
                src_rect.y = 82;
                src_rect.w = 93 - 32;
                src_rect.h = 91 - 82;
            }

            dst_rect.x = (LOGICAL_SCREEN_W / 2) - (src_rect.w / 2);
            dst_rect.y = LOGICAL_SCREEN_H - src_rect.h * 2;
            dst_rect.w = src_rect.w;
            dst_rect.h = src_rect.h;

            SDL_RenderCopy(m_renderer, m_sprite_sheet, &src_rect, &dst_rect);
        }
    }
};

class GameScene : public IScene
{
public:
    GameScene(GameState &game_state, SDL_Renderer *renderer, SDL_Texture *sprite_sheet, const SoundEffects &sfx) noexcept
        : m_game_state{game_state}, m_renderer{renderer}, m_sprite_sheet{sprite_sheet}, m_sfx{sfx}, m_sec_per_tick{SEC_PER_TICK_START}, m_tick_timer{SEC_PER_TICK_START}
    {
    }
    ~GameScene() noexcept override = default;
    GameScene(const GameScene &) noexcept = delete;
    GameScene(GameScene &&) noexcept = delete;
    GameScene operator=(const GameScene &) noexcept = delete;
    GameScene operator=(GameScene &&) noexcept = delete;

public:
    bool update(double dt_sec) override
    {
        bool game_over{false};

        // update santa direction based on WASD or arrow keys
        {
            const Uint8 *keyboard{SDL_GetKeyboardState(nullptr)};
            if (keyboard[SDL_SCANCODE_W] || keyboard[SDL_SCANCODE_UP])
            {
                // santa cannot go in the opposite direction while carrying bags, otherwise he would die
                if (!(m_game_state.scene.prev_tick_direction == DIRECTION_SOUTH && m_game_state.scene.num_bags > 0))
                {
                    m_game_state.scene.cur_tick_direction = DIRECTION_NORTH;
                }
            }
            if (keyboard[SDL_SCANCODE_S] || keyboard[SDL_SCANCODE_DOWN])
            {
                // santa cannot go in the opposite direction while carrying bags, otherwise he would die
                if (!(m_game_state.scene.prev_tick_direction == DIRECTION_NORTH && m_game_state.scene.num_bags > 0))
                {
                    m_game_state.scene.cur_tick_direction = DIRECTION_SOUTH;
                }
            }
            if (keyboard[SDL_SCANCODE_A] || keyboard[SDL_SCANCODE_LEFT])
            {
                // santa cannot go in the opposite direction while carrying bags, otherwise he would die
                if (!(m_game_state.scene.prev_tick_direction == DIRECTION_EAST && m_game_state.scene.num_bags > 0))
                {
                    m_game_state.scene.cur_tick_direction = DIRECTION_WEST;
                }
            }
            if (keyboard[SDL_SCANCODE_D] || keyboard[SDL_SCANCODE_RIGHT])
            {
                // santa cannot go in the opposite direction while carrying bags, otherwise he would die
                if (!(m_game_state.scene.prev_tick_direction == DIRECTION_WEST && m_game_state.scene.num_bags > 0))
                {
                    m_game_state.scene.cur_tick_direction = DIRECTION_EAST;
                }
            }
        }

        // update and render game state
        if (m_tick_timer >= m_sec_per_tick)
        {
            // update game
            game_over = update_game_state(m_game_state, m_sfx);
            // decrease sec per tick
            m_sec_per_tick -= m_sec_per_tick * SEC_PER_TICK_DIFFICULTY_COEFFICIENT;
            m_sec_per_tick = std::max(MIN_SEC_PER_TICK, m_sec_per_tick);
            // reset timer
            m_tick_timer = 0.0;
        }

        // update spawn timer
        m_game_state.scene.spawn_timer += dt_sec;

        // update tick timer
        m_tick_timer += dt_sec;

        return game_over;
    }

    void render() override
    {
        // clear the screen to black
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
        SDL_RenderClear(m_renderer);

        // draw background
        {
            // set background to palette dark green (bounding boxes)
            SDL_SetRenderDrawColor(m_renderer, 31, 50, 36, 255);
            // define the area to draw
            SDL_Rect myRect{0, 0, LOGICAL_SCREEN_W, LOGICAL_SCREEN_H};
            // draw it
            SDL_RenderFillRect(m_renderer, &myRect);
        }

        // render map
        for (int row{}; row < MAP_SIDE; row++)
        {
            for (int col{}; col < MAP_SIDE; col++)
            {
                SDL_Rect dst_rect{};
                dst_rect.x = col * TILE_PIXEL_SIZE;
                dst_rect.y = row * TILE_PIXEL_SIZE;
                dst_rect.w = TILE_PIXEL_SIZE;
                dst_rect.h = TILE_PIXEL_SIZE;

                SDL_Rect src_rect{};
                src_rect.w = TILE_PIXEL_SIZE;
                src_rect.h = TILE_PIXEL_SIZE;

                bool should_render{true};

                switch (m_game_state.scene.map[row][col].type)
                {
                case TILE_GIFT:
                {
                    src_rect.x = 1;
                    src_rect.y = 16;
                }
                break;
                case TILE_BAG:
                {
                    src_rect.x = 16;
                    src_rect.y = 1;
                }
                break;
                case TILE_HOUSE:
                {
                    src_rect.x = 16;
                    src_rect.y = 16;
                }
                break;
                case TILE_EMPTY:
                default:
                {
                    should_render = false;
                }
                break;
                }

                if (should_render)
                {
                    SDL_RendererFlip horizontal_flip = SDL_FLIP_NONE;
                    if (m_game_state.scene.cur_tick_direction == DIRECTION_EAST &&
                        m_game_state.scene.map[row][col].type == TILE_BAG)
                    {
                        horizontal_flip = SDL_FLIP_HORIZONTAL;
                    }

                    SDL_RenderCopyEx(
                        m_renderer,
                        m_sprite_sheet,
                        &src_rect,
                        &dst_rect,
                        0.0,
                        NULL,
                        horizontal_flip);
                }
            }
        }

        // render santa
        {
            SDL_Rect dst_rect{};
            dst_rect.x = m_game_state.scene.santa.col * TILE_PIXEL_SIZE;
            dst_rect.y = m_game_state.scene.santa.row * TILE_PIXEL_SIZE;
            dst_rect.w = TILE_PIXEL_SIZE;
            dst_rect.h = TILE_PIXEL_SIZE;

            SDL_Rect src_rect{};
            src_rect.x = 1;
            src_rect.y = 1;
            src_rect.w = TILE_PIXEL_SIZE;
            src_rect.h = TILE_PIXEL_SIZE;

            SDL_RendererFlip horizontal_flip = SDL_FLIP_NONE;
            if (m_game_state.scene.cur_tick_direction == DIRECTION_EAST)
            {
                horizontal_flip = SDL_FLIP_HORIZONTAL;
            }

            SDL_RenderCopyEx(
                m_renderer,
                m_sprite_sheet,
                &src_rect,
                &dst_rect,
                0.0,
                NULL,
                horizontal_flip);
        }

        // render score ui
        {
            SDL_Rect dst_rect{};

            // render score header
            {
                dst_rect.x = 0;
                dst_rect.y = MAP_SIDE * TILE_PIXEL_SIZE + SCORE_PIXEL_PAD;
                dst_rect.w = 34;
                dst_rect.h = SCORE_PIXEL_HEIGHT;

                SDL_Rect src_rect{};
                src_rect.x = 32;
                src_rect.y = 1;
                src_rect.w = 36;
                src_rect.h = SCORE_PIXEL_HEIGHT;

                SDL_RenderCopy(m_renderer, m_sprite_sheet, &src_rect, &dst_rect);
            }

            // render score digits, one by one
            {
                constexpr int PIXEL_ADVANCE{1};

                std::array<SDL_Rect, 10> digit_src_rect{};
                digit_src_rect[0] = SDL_Rect{.x = 69, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT};  // 0
                digit_src_rect[1] = SDL_Rect{.x = 74, .y = 1, .w = 3, .h = SCORE_PIXEL_HEIGHT};  // 1
                digit_src_rect[2] = SDL_Rect{.x = 78, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT};  // 2
                digit_src_rect[3] = SDL_Rect{.x = 83, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT};  // 3
                digit_src_rect[4] = SDL_Rect{.x = 88, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT};  // 4
                digit_src_rect[5] = SDL_Rect{.x = 93, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT};  // 5
                digit_src_rect[6] = SDL_Rect{.x = 98, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT};  // 6
                digit_src_rect[7] = SDL_Rect{.x = 103, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT}; // 7
                digit_src_rect[8] = SDL_Rect{.x = 108, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT}; // 8
                digit_src_rect[9] = SDL_Rect{.x = 113, .y = 1, .w = 4, .h = SCORE_PIXEL_HEIGHT}; // 9

                int divisor{100}; // Max score is the largest with 3 digits, i.e. 999.
                while (divisor > 0)
                {
                    int digit{0};
                    if (m_game_state.score / divisor > 0)
                    {
                        // extract most significant digit
                        digit = (m_game_state.score / divisor) % 10;
                    }

                    // render it
                    SDL_Rect src_rect{digit_src_rect.at(static_cast<size_t>(digit))};
                    dst_rect.x += dst_rect.w + PIXEL_ADVANCE;
                    dst_rect.w = src_rect.w;
                    dst_rect.h = src_rect.h;
                    SDL_RenderCopy(m_renderer, m_sprite_sheet, &src_rect, &dst_rect);

                    divisor /= 10;
                }
            }
        }
    }

private:
    GameState &m_game_state;
    SDL_Renderer *m_renderer;
    SDL_Texture *m_sprite_sheet;
    const SoundEffects &m_sfx;
    double m_sec_per_tick;
    double m_tick_timer;
};

static int
entry()
{
    // ------------------------------------------------------------------------
    // sdl2 initialization
    // ------------------------------------------------------------------------

    SDL2ExHandle sdl2ex_handle{};
    SDL2ExImageHandle sdl2ex_image_handle{};
    SDL2ExMixerHandle sdl2ex_mixer_handle{};
    SDL2ExWindow window{};
    SDL2ExRenderer renderer{window.Handle()};

    // ------------------------------------------------------------------------
    // asset loading
    // ------------------------------------------------------------------------

    SDL2ExTexture sprite_sheet{renderer, "assets/cozychristmas.png"};
    SDL2ExMusic theme{"assets/theme.mp3"};
    SDL2ExChunk start{"assets/start.wav"};
    SDL2ExChunk gift{"assets/gift.wav"};
    SDL2ExChunk house{"assets/house.wav"};
    SDL2ExChunk hurt{"assets/hurt.wav"};
    SDL2ExChunk step{"assets/step.wav"};
    SDL2ExChunk spawn{"assets/spawn.wav"};

    // ------------------------------------------------------------------------
    // main loop
    // ------------------------------------------------------------------------

    // set logical screen size
    {
        int res{SDL_RenderSetLogicalSize(renderer.Handle(), LOGICAL_SCREEN_W, LOGICAL_SCREEN_H)};
        if (res < 0)
        {
            error(std::format("failed to set logical screen size to {}x{}", LOGICAL_SCREEN_W, LOGICAL_SCREEN_H));
        }
    }

    bool should_exit{false};
    GameState game_state{};

    SoundEffects sfx{start.Handle(), gift.Handle(), house.Handle(), hurt.Handle(), step.Handle(), spawn.Handle()};

    // start playing music (loops: -1 = infinite)
    Mix_PlayMusic(theme.Handle(), -1);
    Mix_VolumeMusic(16); // [0,128] // TODO: not here

    GameStartScene game_start{game_state, renderer.Handle(), sprite_sheet.Handle(), sfx};
    GameScene game_scene{game_state, renderer.Handle(), sprite_sheet.Handle(), sfx};
    EndGameScene end_game_scene{game_state, renderer.Handle(), sprite_sheet.Handle(), sfx};
    std::array<IScene *, 3> scenes{&game_start, &game_scene, &end_game_scene};
    int current_scene_idx{};

    Uint64 last_frame_start{SDL_GetPerformanceCounter()};
    bool game_over{};
    while (!should_exit)
    {
        // compute last frame delta time
        Uint64 this_frame_start{SDL_GetPerformanceCounter()};
        double dt_sec{static_cast<double>((this_frame_start - last_frame_start)) / static_cast<double>(SDL_GetPerformanceFrequency())};
        last_frame_start = this_frame_start;

        // process input
        {
            // run message pump
            SDL_Event e;
            while (SDL_PollEvent(&e) != 0)
            {
                if (e.type == SDL_QUIT)
                {
                    // user requests quit
                    should_exit = true;
                }
                else if (e.type == SDL_KEYDOWN)
                {
                    // key presses events
                    switch (e.key.keysym.sym)
                    {
                    case SDLK_ESCAPE:
                    {
                        should_exit = true;
                    }
                    break;
                    default:
                    {
                        // do nothing
                    }
                    }
                }

                IScene *scene{scenes[static_cast<size_t>(current_scene_idx)]};
                scene->on_event(e);
            }
        }

        // switch scene
        if (game_over)
        {
            game_state.scene = SceneGameState{};
            current_scene_idx = (current_scene_idx + 1) % static_cast<int>(scenes.size());
            current_scene_idx = current_scene_idx == 0 ? current_scene_idx + 1 : current_scene_idx;

            // if the upcoming scene is the game scene we reset the score
            if (auto it{std::find(scenes.begin(), scenes.end(), &game_scene)}; it != scenes.end())
            {
                int game_scene_idx{static_cast<int>(std::distance(scenes.begin(), it))};
                if (current_scene_idx == game_scene_idx)
                {
                    game_state.score = 0;
                }
            }
            else
            {
                error("unable to find game scene in scenes array");
            }
        }

        // update and render current scene
        {
            IScene *scene{scenes[static_cast<size_t>(current_scene_idx)]};
            game_over = scene->update(dt_sec);
            scene->render();
        }

        // present
        SDL_RenderPresent(renderer.Handle());
    }

    return 0;
}

int main()
{
    try
    {
        entry();
    }
    catch (const Error &error)
    {
        std::cerr << error.what() << "\n";
    }

    return 1;
}
