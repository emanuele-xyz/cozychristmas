#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>

#include <vector>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <format>
#include <random>
#include <stacktrace>
#include <stacktrace>
#include <string>
#include <stdexcept>

constexpr int SCREEN_W{720};
constexpr int SCREEN_H{720};
constexpr int MAP_SIDE{8};
constexpr int TILE_PIXEL_SIZE{14}; // TODO: find a better name
constexpr int LOGICAL_SCREEN_W{MAP_SIDE * TILE_PIXEL_SIZE};
constexpr int LOGICAL_SCREEN_H{MAP_SIDE * TILE_PIXEL_SIZE};
constexpr double SPAWN_TIME_SEC_START{2.0};
constexpr double SPAWN_TIME_DIFFICULTY_COEFFICIENT{0.01};
constexpr double MIN_SPAWN_TIME_SEC{0.5};
constexpr double SEC_PER_TICK{0.5};

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
    // TODO: there should be a cleaner solution here, but right now I don't care enough
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
    static std::random_device random_device{};
#pragma clang diagnostic pop
    static std::mt19937 generator{random_device()};
    std::uniform_int_distribution<> distribution{lo, hi}; // from 'lo' included to 'hi' included
    return distribution(generator);
}

#if 0
static constexpr const char *direction_to_str(Direction direction) noexcept
{
    const char *str{""};
    switch (direction)
    {
    case DIRECTION_NORTH:
    {
        str = "NORTH";
    }
    break;
    case DIRECTION_SOUTH:
    {
        str = "SOUTH";
    }
    break;
    case DIRECTION_WEST:
    {
        str = "WEST";
    }
    break;
    case DIRECTION_EAST:
    {
        str = "EAST";
    }
    break;
    default:
    {
    }
    }
    return str;
}
#endif

static int mod(int a, int b)
{
    return (a % b + b) % b;
}

struct GameState
{
    Tile map[MAP_SIDE][MAP_SIDE]{};
    Direction santa_direction;
    v2 santa;
    int num_bags;
    v2 first_bag;
    v2 last_bag;
    bool game_over{true};
    bool exit;
    double spawn_time_sec{SPAWN_TIME_SEC_START};
    double spawn_timer{};
};

struct SoundEffects
{
    Mix_Chunk *gift;
    Mix_Chunk *house;
    Mix_Chunk *hurt;
    Mix_Chunk *step;
    Mix_Chunk *spawn;
};

static void init_game_state(GameState &)
{
    // TODO: default initialize map, direction, santa, num_bags, first_bag, last_bag, spwan time sec, spwan timer
}

static void update_game_state(GameState &state, const SoundEffects &sfx)
{
    // save old santa position for later
    v2 old_santa{state.santa};

    // move santa
    switch (state.santa_direction)
    {
    case DIRECTION_NORTH:
    {
        state.santa.row = mod(state.santa.row - 1, MAP_SIDE);
    }
    break;
    case DIRECTION_SOUTH:
    {
        state.santa.row = mod(state.santa.row + 1, MAP_SIDE);
    }
    break;
    case DIRECTION_WEST:
    {
        state.santa.col = mod(state.santa.col - 1, MAP_SIDE);
    }
    break;
    case DIRECTION_EAST:
    {
        state.santa.col = mod(state.santa.col + 1, MAP_SIDE);
    }
    break;
    default:
    {
        unreachable();
    }
    }

    // run custom logic based on which tile santa is on
    switch (state.map[state.santa.row][state.santa.col].type)
    {
    case TILE_EMPTY:
    {
        // if there are bags, employ logic to move them
        if (state.num_bags > 0)
        {
            // make bag where santa was
            state.map[old_santa.row][old_santa.col] = Tile{TILE_BAG};
            // save old first bag location
            v2 old_first_bag{state.first_bag};
            // set new first bag location to where santa was
            state.first_bag = old_santa;
            // update previous bag pointer for old fisrst bag
            state.map[old_first_bag.row][old_first_bag.col] = Tile{TILE_BAG, old_santa.row, old_santa.col};
            // save previous to last bag position
            v2 previous_to_last_bag{state.map[state.last_bag.row][state.last_bag.col].prev_row, state.map[state.last_bag.row][state.last_bag.col].prev_col};
            // make empty tile where the last bag is
            state.map[state.last_bag.row][state.last_bag.col] = Tile{TILE_EMPTY};
            // update last bag position to previos to last bag position
            state.last_bag = previous_to_last_bag;
        }

        // play step sound effect
        Mix_PlayChannel(-1, sfx.step, 0);
    }
    break;
    case TILE_GIFT:
    {
        // make gift tile empty where santa is
        state.map[state.santa.row][state.santa.col] = Tile{TILE_EMPTY};
        // spawn bag where santa was
        state.map[old_santa.row][old_santa.col] = Tile{TILE_BAG};
        if (state.num_bags <= 0)
        {
            // set first and last bag positions to where santa was
            state.first_bag = old_santa;
            state.last_bag = old_santa;
        }
        else // state.num_bags > 0
        {
            // update former first bag previous pointer with the new first bag
            state.map[state.first_bag.row][state.first_bag.col] = Tile{TILE_BAG, old_santa.row, old_santa.col};
        }
        // update new first bag position
        state.first_bag = old_santa;
        // increase number of bags
        state.num_bags++;

        // play gift sound effect
        Mix_PlayChannel(-1, sfx.gift, 0);
    }
    break;
    case TILE_BAG:
    {
        state.game_over = true;
    }
    break;
    case TILE_HOUSE:
    {
        if (state.num_bags <= 0)
        {
            state.game_over = true;

            // play hurt sound effect
            Mix_PlayChannel(-1, sfx.hurt, 0);
        }
        else // state.num_bags > 0
        {
            // make house tile empty where santa is
            state.map[state.santa.row][state.santa.col] = Tile{TILE_EMPTY};
            // save last bag tile
            Tile saved = state.map[state.last_bag.row][state.last_bag.col];
            // remove last bag tile
            state.map[state.last_bag.row][state.last_bag.col] = Tile{TILE_EMPTY};
            if (state.num_bags > 1)
            {
                // update last bag
                state.last_bag = v2{saved.prev_row, saved.prev_col};
                // spawn bag where santa was
                state.map[old_santa.row][old_santa.col] = Tile{TILE_BAG};
                // save former first bag
                v2 old_first_bag = state.first_bag;
                // update new first bag position
                state.first_bag = old_santa;
                // update former first bag previous pointer with the new first bag
                state.map[old_first_bag.row][old_first_bag.col] = Tile{TILE_BAG, state.first_bag.row, state.first_bag.col};
                // save previous to last bag
                Tile last_bag{state.map[state.last_bag.row][state.last_bag.col]};
                // remove last bag tile
                state.map[state.last_bag.row][state.last_bag.col] = Tile{TILE_EMPTY};
                // update last bag position
                state.last_bag = v2{last_bag.prev_row, last_bag.prev_col};
            }
            // decrease number of bags
            state.num_bags--;

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
    if (state.spawn_timer >= state.spawn_time_sec)
    {
        // spawning logic
        {
            std::vector<v2> empty_tiles{};
            for (int row{}; row < MAP_SIDE; row++)
            {
                for (int col{}; col < MAP_SIDE; col++)
                {
                    bool is_santa_on_tile{state.santa.row == row && state.santa.col == col};
                    if (state.map[row][col].type == TILE_EMPTY && !is_santa_on_tile)
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
                state.map[random_tile.row][random_tile.col] = Tile{random_int(1, 100) <= 50 ? TILE_GIFT : TILE_HOUSE};

                // play spawn sound
                Mix_PlayChannel(-1, sfx.spawn, 0);
            }
        }

        // make spawn time a little shorter (to make game harder)
        state.spawn_time_sec -= SPAWN_TIME_DIFFICULTY_COEFFICIENT * state.spawn_time_sec;
        // make sure spawn time doesn't go below the fixed minimum
        state.spawn_time_sec = std::max(state.spawn_time_sec, MIN_SPAWN_TIME_SEC);
        // reset timer
        state.spawn_timer = 0.0;
    }
}

#if 0
static void entry()
{
    GameState game_state{};
    init_game_state(game_state);

    while (!game_state.game_over)
    {
        // render game state
        {
            for (int i{}; i < MAP_SIDE; i++)
            {
                for (int j{}; j < MAP_SIDE; j++)
                {
                    // if the tile is the one of which santa is, render santa
                    if (game_state.santa.row == i && game_state.santa.col == j)
                    {
                        std::cout << '<';
                    }
                    else // otherwise, render the tile
                    {
                        switch (game_state.map[i][j].type)
                        {
                        case TILE_EMPTY:
                            std::cout << '.';
                            break;
                        case TILE_BAG:
                            std::cout << '@';
                            break;
                        case TILE_GIFT:
                            std::cout << '$';
                            break;
                        case TILE_HOUSE:
                            std::cout << '^';
                            break;
                        default:
                            unreachable();
                        }
                    }
                }
                std::cout << "\n";
            }

            std::cout << "direction: " << direction_to_str(game_state.santa_direction) << '\n';
            std::cout << "Santa: " << game_state.santa.row << ',' << game_state.santa.col << '\n';
            std::cout << "Bags: " << game_state.num_bags << '\n';
            std::cout << "First bag: " << game_state.first_bag.row << ',' << game_state.first_bag.col << '\n';
            std::cout << "Last bag: " << game_state.last_bag.row << ',' << game_state.last_bag.col << '\n';
        }

        // get input for current tick
        char direction_input{};
        std::cout << "action: ";
        std::cin >> direction_input;

        // translate input to direction or exit
        Direction direction{};
        switch (direction_input)
        {
        case 'n':
        {
            direction = DIRECTION_NORTH;
        }
        break;
        case 's':
        {
            direction = DIRECTION_SOUTH;
        }
        break;
        case 'e':
        {
            direction = DIRECTION_EAST;
        }
        break;
        case 'w':
        {
            direction = DIRECTION_WEST;
        }
        break;
        default:
        {
            game_state.game_over = true;
        }
        break;
        }

        // update game state with new direct
        game_state.santa_direction = direction;

        // update game state
        update_game_state(game_state);
    }
}
#endif

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
    virtual void update(double dt_sec) = 0;
    virtual void render() = 0;

public:
    virtual ~IScene() noexcept = default;
};

class GameOverScene : public IScene
{
public:
    GameOverScene(GameState &game_state, SDL_Renderer *renderer, SDL_Texture *sprite_sheet) noexcept
        : m_game_state{game_state}, m_renderer{renderer}, m_sprite_sheet{sprite_sheet} {}
    ~GameOverScene() noexcept override = default;
    GameOverScene(const GameOverScene &) noexcept = delete;
    GameOverScene(GameOverScene &&) noexcept = delete;
    GameOverScene &operator=(const GameOverScene &) noexcept = delete;
    GameOverScene &operator=(GameOverScene &&) noexcept = delete;

public:
    void update(double /*dt_sec*/) override
    {
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

        constexpr int cozy_christmas_w{109};
        constexpr int cozy_christmas_h{43};

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

private:
    [[maybe_unused]] GameState &m_game_state; // TODO: remove [[maybe_unused]]
    SDL_Renderer *m_renderer;
    SDL_Texture *m_sprite_sheet;
};

class GameScene : public IScene
{
public:
    GameScene(GameState &game_state, SDL_Renderer *renderer, SDL_Texture *sprite_sheet, const SoundEffects &sfx) noexcept
        : m_game_state{game_state}, m_renderer{renderer}, m_sprite_sheet{sprite_sheet}, m_sfx{sfx}, m_tick_timer{SEC_PER_TICK}
    {
    }
    ~GameScene() noexcept override = default;
    GameScene(const GameScene &) noexcept = delete;
    GameScene(GameScene &&) noexcept = delete;
    GameScene operator=(const GameScene &) noexcept = delete;
    GameScene operator=(GameScene &&) noexcept = delete;

public:
    void update(double dt_sec) override
    {
        // update santa direction based on WASD or arrow keys
        {
            const Uint8 *keyboard{SDL_GetKeyboardState(nullptr)};
            if (keyboard[SDL_SCANCODE_W] || keyboard[SDL_SCANCODE_UP])
            {
                // santa cannot go in the opposite direction while carrying bags, otherwise he would die
                if (!(m_game_state.santa_direction == DIRECTION_SOUTH && m_game_state.num_bags > 0))
                {
                    m_game_state.santa_direction = DIRECTION_NORTH;
                }
            }
            if (keyboard[SDL_SCANCODE_S] || keyboard[SDL_SCANCODE_DOWN])
            {
                // santa cannot go in the opposite direction while carrying bags, otherwise he would die
                if (!(m_game_state.santa_direction == DIRECTION_NORTH && m_game_state.num_bags > 0))
                {
                    m_game_state.santa_direction = DIRECTION_SOUTH;
                }
            }
            if (keyboard[SDL_SCANCODE_A] || keyboard[SDL_SCANCODE_LEFT])
            {
                // santa cannot go in the opposite direction while carrying bags, otherwise he would die
                if (!(m_game_state.santa_direction == DIRECTION_EAST && m_game_state.num_bags > 0))
                {
                    m_game_state.santa_direction = DIRECTION_WEST;
                }
            }
            if (keyboard[SDL_SCANCODE_D] || keyboard[SDL_SCANCODE_RIGHT])
            {
                // santa cannot go in the opposite direction while carrying bags, otherwise he would die
                if (!(m_game_state.santa_direction == DIRECTION_WEST && m_game_state.num_bags > 0))
                {
                    m_game_state.santa_direction = DIRECTION_EAST;
                }
            }
        }

        // update and render game state
        if (m_tick_timer >= SEC_PER_TICK)
        {
            // update game
            update_game_state(m_game_state, m_sfx);

            // reset timer
            m_tick_timer = 0.0;
        }

        // update spawn timer
        m_game_state.spawn_timer += dt_sec;

        // update tick timer
        m_tick_timer += dt_sec;
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

                switch (m_game_state.map[row][col].type)
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
                    if (m_game_state.santa_direction == DIRECTION_EAST &&
                        m_game_state.map[row][col].type == TILE_BAG)
                    {
                        SDL_RenderCopyEx(
                            m_renderer,
                            m_sprite_sheet,
                            &src_rect,
                            &dst_rect,
                            0.0,
                            NULL,
                            SDL_FLIP_HORIZONTAL);
                    }
                    else
                    {
                        SDL_RenderCopy(m_renderer, m_sprite_sheet, &src_rect, &dst_rect);
                    }
                }
            }
        }

        // render santa
        {
            SDL_Rect dst_rect{};
            dst_rect.x = m_game_state.santa.col * TILE_PIXEL_SIZE;
            dst_rect.y = m_game_state.santa.row * TILE_PIXEL_SIZE;
            dst_rect.w = TILE_PIXEL_SIZE;
            dst_rect.h = TILE_PIXEL_SIZE;

            SDL_Rect src_rect{};
            src_rect.x = 1;
            src_rect.y = 1;
            src_rect.w = TILE_PIXEL_SIZE;
            src_rect.h = TILE_PIXEL_SIZE;

            if (m_game_state.santa_direction == DIRECTION_EAST)
            {
                SDL_RenderCopyEx(
                    m_renderer,
                    m_sprite_sheet,
                    &src_rect,
                    &dst_rect,
                    0.0,
                    NULL,
                    SDL_FLIP_HORIZONTAL);
            }
            else
            {
                SDL_RenderCopy(m_renderer, m_sprite_sheet, &src_rect, &dst_rect);
            }
        }

        // TODO: render ui
    }

private:
    GameState &m_game_state;
    SDL_Renderer *m_renderer;
    SDL_Texture *m_sprite_sheet;
    const SoundEffects &m_sfx;
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

    GameState game_state{};
    init_game_state(game_state);

    SoundEffects sfx{gift.Handle(), house.Handle(), hurt.Handle(), step.Handle(), spawn.Handle()};

    // start playing music (loops: -1 = infinite)
    Mix_PlayMusic(theme.Handle(), -1);
    Mix_VolumeMusic(16); // [0,128] // TODO: not here

    GameScene game_scene{game_state, renderer.Handle(), sprite_sheet.Handle(), sfx};
    GameOverScene game_over_scene{game_state, renderer.Handle(), sprite_sheet.Handle()};
    // IScene *current_scene{&game_over_scene};
    IScene *current_scene{&game_scene};

    Uint64 last_frame_start{SDL_GetPerformanceCounter()};
    while (!game_state.exit)
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
                    game_state.exit = true;
                }
                else if (e.type == SDL_KEYDOWN)
                {
                    // key presses events
                    switch (e.key.keysym.sym)
                    {
                    case SDLK_RETURN:
                    {
                        game_state.game_over = false; // TODO: not correct
                    }
                    break;
                    case SDLK_ESCAPE:
                    {
                        game_state.exit = true;
                    }
                    break;
                    default:
                    {
                        // do nothing
                    }
                    }
                }
            }
        }

        // switch scene
        if (game_state.game_over)
        {
            current_scene = &game_over_scene;
        }
        else
        {
            init_game_state(game_state);
            current_scene = &game_scene;
        }

        // update scene
        current_scene->update(dt_sec);

        // render scene
        current_scene->render();

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
