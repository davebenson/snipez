/* GOAL: - allow numerous snipe sessions to occur.
        
   PROTOCOL:
     - client connects to server to get a list of games
     - client either starts a new game, or joins an existing game
     - client connects repeatedly (hopefully w/ keepalive)
       to receive a new screen state and tell the server of user actions

   PATHS:
     /        -- dispense JS
     /games   -- retrieve a list of games
     /newgame -- create a new game
     /join    -- join a game, receive init screen
     /update  -- offer key info, update screen
     /leave   -- leave a game
 */

/* size of a single tile in pixels */
#define TILE_SIZE       8

/* size of the cell in the maze, in tiles */
#define CELL_SIZE       20

#include "../../dsk/dsk.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* XXX TODO: use better random number generator */
static unsigned random_int_range (unsigned max)
{
  return rand() % max;
}

typedef struct _User User;
typedef struct _Enemy Enemy;
typedef struct _Bullet Bullet;
typedef struct _Generator Generator;
typedef struct _Cell Cell;
typedef struct _Game Game;

typedef struct _PendingUpdate PendingUpdate;

struct _User
{
  char *name;
  unsigned x, y;
  unsigned width, height;               /* canvas width, height */

  unsigned last_seen_time;
  Game *game;
  User *next_in_game, *prev_in_game;
  User *next_in_cell, *prev_in_cell;
  int move_x, move_y;

  /* if you connect and you already have gotten the latest
     screen, we make you wait for the next update. */
  unsigned last_update;
};

struct _Enemy
{
  unsigned x, y;
  Enemy *prev_in_game, *next_in_game;
  Enemy *prev_in_cell, *next_in_cell;
};

struct _Bullet
{
  unsigned x, y;
  int delta_x, delta_y;
  Bullet *next_in_cell, *prev_in_cell;;
  Bullet *next_global;
};

struct _Generator
{
  Game *game;
  unsigned x, y;
  double generator_prob;
  Generator *next;
};

struct _Cell
{
  Bullet *bullets;
  Enemy *enemies;
  User *users;
  Generator *generator;
};

struct _Game
{
  char *name;
  Game *next_game;

  unsigned universe_width, universe_height;     // in cells
  uint8_t *h_walls;             /* universe_height x universe_width */
  uint8_t *v_walls;             /* universe_height x universe_width */

  User *users;

  Cell *cells;               /* universe_height x universe_width */
  Bullet *all_bullets;
  dsk_boolean wrap;

  unsigned latest_update;
  PendingUpdate *pending_updates;
};
static Game *all_games;

struct _PendingUpdate
{
  User *user;
  DskHttpServerRequest *request;
  PendingUpdate *next;
};

/* --- Creating a new game --- */
static uint8_t *generate_ones (unsigned count)
{
  return memset (dsk_malloc (count), 1, count);
}

/* height * width * 2 [v, h] */
typedef struct _TmpWall TmpWall;
struct _TmpWall
{
  /* walls that separate different sets */
  TmpWall *prev, *next;
};

typedef struct _TmpSetInfo TmpSetInfo;
struct _TmpSetInfo
{
  unsigned set_number;
  TmpSetInfo *next_in_set;              /* NOTE: a ring */
};

static void
remove_tmp_wall (Game *game,
                 TmpWall *tmp_walls,
                 unsigned index,
                 TmpWall **wall_list_inout)

{
  TmpWall *remove = tmp_walls + index;
  unsigned x = (index / 2) % game->universe_width;
  unsigned y = (index / 2) / game->universe_width;
  unsigned h = index % 2;
  if (remove->prev == NULL)
    {
      dsk_assert (*wall_list_inout == remove);
      (*wall_list_inout) = remove->next;
    }
  else
    remove->prev->next = remove->next;
  if (remove->next != NULL)
    remove->next->prev = remove->prev;
  remove->prev = remove->next = (void*) 1;
  if (h)
    game->h_walls[x + y * game->universe_width] = 0;
  else
    game->v_walls[x + y * game->universe_width] = 0;
}


static void swap_ints (unsigned *a, unsigned *b)
{
  unsigned swap = *a;
  *a = *b;
  *b = swap;
}

static Game *
create_game (const char *name)
{
  Game *game = dsk_malloc (sizeof (Game));
  unsigned usize;

  game->name = dsk_strdup (name);
  game->next_game = all_games;
  all_games = game;
  game->universe_width = 32;
  game->universe_height = 32;
  usize = game->universe_height * game->universe_width;
  game->h_walls = generate_ones (usize);
  game->v_walls = generate_ones (usize);
  game->users = NULL;
  game->cells = dsk_malloc0 (sizeof (Cell) * game->universe_width * game->universe_height);
  game->latest_update = 0;
  game->wrap = DSK_TRUE;

  /* Generate with modified kruskals algorithm */
  usize = game->universe_width * game->universe_height;
  TmpWall *tmp_walls = dsk_malloc (sizeof (TmpWall) * usize * 2);
  TmpSetInfo *sets = dsk_malloc (sizeof (TmpSetInfo) * usize);

  /* connect the walls together in random order */
  unsigned *scramble, i;
  scramble = dsk_malloc (sizeof (unsigned) * usize * 2);
  for (i = 0; i < usize * 2; i++)
    scramble[i] = i;
  for (i = 0; i < usize * 2; i++)
    swap_ints (scramble + random_int_range (usize * 2), scramble + random_int_range (usize * 2));

  TmpWall *wall_list = NULL;
  for (i = 0; i < game->universe_width * game->universe_height; i++)
    {
      unsigned e = scramble[i];
      unsigned h = e % 2;
      unsigned x = (e / 2) % game->universe_width;
      unsigned y = e / (game->universe_width * 2);
      if (!game->wrap)
        {
          if ((h && y == 0) || (!h && x == 0))
            continue;
        }
      tmp_walls[e].prev = NULL;
      tmp_walls[e].next = wall_list;
      if (wall_list)
        wall_list->prev = tmp_walls + e;
      wall_list = tmp_walls + e;
    }

  for (i = 0; i < usize; i++)
    {
      sets[i].set_number = i;
      sets[i].next_in_set = sets + i;
    }

  while (wall_list != NULL)
    {
      /* remove wall */
      unsigned e = wall_list - tmp_walls;
      unsigned h = e % 2;
      unsigned x = (e / 2) % game->universe_width;
      unsigned y = e / (game->universe_width * 2);
      TmpSetInfo *si = sets + e / 2;
      TmpSetInfo *osi;
      if (h)
        {
          if (x == 0)
            osi = si + game->universe_width - 1;
          else
            osi = si - 1;
        }
      else
        {
          if (y == 0)
            osi = si + (game->universe_height - 1) * game->universe_width;
          else
            osi = si - game->universe_width;
        }
      dsk_assert (osi->set_number != si->set_number);
      TmpSetInfo *kring = osi->set_number < si->set_number ? osi : si;              /* ring to keep */
      TmpSetInfo *dring = osi->set_number < si->set_number ? si : osi;              /* ring to change */
      TmpSetInfo *dring_start = dring;

      /* combine sets (removing any walls that no longer separate different sets
         from the list of walls to remove) */
      do
        {
          unsigned x = (dring - sets) % game->universe_width;
          unsigned y = (dring - sets) / game->universe_width;
          int wall_idx;
          dring->set_number = kring->set_number;

          /* Maybe remove left wall from candidate set of walls. */
          wall_idx = -1;
          if (x > 0 && (dring-1)->set_number == dring->set_number)
            wall_idx = 2 * (x + y * game->universe_width);
          else if (x == 0 && game->wrap && (dring+game->universe_width-1)->set_number == kring->set_number)
            wall_idx = 2 * (x + y * game->universe_width);
          if (wall_idx >= 0)
            remove_tmp_wall (game, tmp_walls, wall_idx, &wall_list);

          /* Maybe remove right wall from candidate set of walls. */
          wall_idx = -1;
          if (x < game->universe_width - 1 && (dring+1)->set_number == dring->set_number)
            wall_idx = 2 * ((x+1) + y * game->universe_width);
          else if (x == game->universe_width - 1 && game->wrap && (dring-game->universe_width+1)->set_number == kring->set_number)
            wall_idx = 2 * (0 + y * game->universe_width);
          if (wall_idx >= 0)
            remove_tmp_wall (game, tmp_walls, wall_idx, &wall_list);

          /* Maybe remove top wall from candidate set of walls. */
          wall_idx = -1;
          if (y > 0 && (dring-game->universe_width)->set_number == dring->set_number)
            wall_idx = 2 * (x + y * game->universe_width) + 1;
          else if (y == 0 && game->wrap && (dring+game->universe_width*(game->universe_height-1))->set_number == kring->set_number)
            wall_idx = 2 * (x + y * game->universe_width) + 1;
          if (wall_idx >= 0)
            remove_tmp_wall (game, tmp_walls, wall_idx, &wall_list);

          /* Maybe remove bottom wall from candidate set of walls. */
          wall_idx = -1;
          if (x < game->universe_width - 1 && (dring+1)->set_number == dring->set_number)
            wall_idx = 2 * ((x+1) + y * game->universe_width) + 1;
          else if (x == game->universe_width - 1 && game->wrap && (dring-game->universe_width+1)->set_number == kring->set_number)
            wall_idx = 2 * (0 + y * game->universe_width) + 1;
          if (wall_idx >= 0)
            remove_tmp_wall (game, tmp_walls, wall_idx, &wall_list);

          dring = dring->next_in_set;
        }
      while (dring != dring_start);

      /* Merge the rings */
      TmpSetInfo *old_dring_next = dring->next_in_set;
      dring->next_in_set = kring->next_in_set;
      kring->next_in_set = old_dring_next;
    }

  dsk_free (tmp_walls);
  dsk_free (sets);
  return game;
}

/* --- getting the occupancy of a x,y position --- */
typedef enum
{
  OCC_EMPTY,
  OCC_WALL,
  OCC_USER,
  OCC_ENEMY,
  OCC_BULLET,
  OCC_GENERATOR
} OccType;

/* *ptr_out will be set in the following cases:
    case        type
    -----       ----
    BULLET      Bullet
    USER        User
    ENEMY       Enemy
    GENERATOR   Generator
 */
static OccType
get_occupancy (Game *game, unsigned x, unsigned y, void **ptr_out)
{
  Cell *cell;
  User *user;
  Bullet *bullet;
  Enemy *enemy;
  if (x >= CELL_SIZE * game->universe_width
   || y >= CELL_SIZE * game->universe_height)
    return OCC_WALL;
  cell = game->cells + (x / CELL_SIZE) + (y / CELL_SIZE) * game->universe_width;
  for (user = cell->users; user; user = user->next_in_cell)
    if (user->x == x && user->y == y)
      {
        *ptr_out = user;
        return OCC_USER;
      }
  if (cell->generator
      && (cell->generator->x == x || cell->generator->x + 1 == x)
      && (cell->generator->y == y || cell->generator->y + 1 == y))
    {
      *ptr_out = cell->generator;
      return OCC_GENERATOR;
    }
  for (bullet = cell->bullets; bullet; bullet = bullet->next_in_cell)
    if (bullet->x == x && bullet->y == y)
      {
        *ptr_out = bullet;
        return OCC_BULLET;
      }
  for (enemy = cell->enemies; enemy; enemy = enemy->next_in_cell)
    if (enemy->x == x && enemy->y == y)
      {
        *ptr_out = enemy;
        return OCC_ENEMY;
      }
  return OCC_EMPTY;
}

/* --- Creating a user in a game --- */
static User *
create_user (Game *game, const char *name, unsigned width, unsigned height)
{
  User *user = dsk_malloc (sizeof (User));
  Cell *cell;
  void *dummy;

  user->name = dsk_strdup (name);

  /* pick random unoccupied position */
  do
    {
      user->x = random_int_range (game->universe_width * CELL_SIZE);
      user->y = random_int_range (game->universe_height * CELL_SIZE);
    }
  while (get_occupancy (game, user->x, user->y, &dummy) != OCC_EMPTY);

  cell = game->cells
       + (user->x / CELL_SIZE)
       + (user->y / CELL_SIZE) * game->universe_width;

  user->next_in_cell = user;
  user->prev_in_cell = NULL;
  cell->users = user;

  user->width = width;
  user->height = height;

  user->next_in_game = game->users;
  user->prev_in_game = NULL;
  game->users = user;

  user->last_seen_time = dsk_dispatch_default ()->last_dispatch_secs;
  user->game = game;
  user->move_x = user->move_y = 0;
  user->last_update = (unsigned)(-1);
  return user;
}

/* --- rendering --- */

static int
int_div (int a, unsigned b)
{
  if (a < 0)
    {
      unsigned A = -a;
      unsigned quot = (A + b - 1) / b;
      return -quot;
    }
  else
    return a/b;
}
static void
append_element_json (unsigned *n_inout,
                     DskJsonValue ***arr_inout,
                     unsigned *alloced_inout,
                     DskJsonValue *value)
{
  if (*n_inout == *alloced_inout)
    {
      *alloced_inout *= 2;
      *arr_inout = dsk_realloc (*arr_inout,
                                sizeof (DskJsonValue*) * *alloced_inout);
    }
  (*arr_inout)[*n_inout] = value;
  *n_inout += 1;
}

static void
add_wall (unsigned *n_inout,
          DskJsonValue ***arr_inout,
          unsigned *alloced_inout,
          int x, int y, unsigned width, unsigned height)
{
  DskJsonMember members[6];
  members[0].name = "x";
  members[0].value = dsk_json_value_new_number (x);
  members[1].name = "y";
  members[1].value = dsk_json_value_new_number (y);
  members[2].name = "width";
  members[2].value = dsk_json_value_new_number (width);
  members[3].name = "height";
  members[3].value = dsk_json_value_new_number (height);
  members[4].name = "color";
  members[4].value = dsk_json_value_new_string (7, "#ffffff");
  members[5].name = "type";
  members[5].value = dsk_json_value_new_string (9, "rectangle");

  append_element_json (n_inout, arr_inout, alloced_inout,
           dsk_json_value_new_object (DSK_N_ELEMENTS (members), members));
}
static void
add_bullet (unsigned *n_inout,
            DskJsonValue ***arr_inout,
            unsigned *alloced_inout,
            int       px,
            int       py)
{
  DskJsonMember members[5];
  members[0].name = "x";
  members[0].value = dsk_json_value_new_number (px);
  members[1].name = "y";
  members[1].value = dsk_json_value_new_number (py);
  members[2].name = "radius";
  members[2].value = dsk_json_value_new_number (TILE_SIZE * 3 / 8);
  members[3].name = "color";
  members[3].value = dsk_json_value_new_string (7, "#ffffff");
  members[4].name = "type";
  members[4].value = dsk_json_value_new_string (6, "circle");
  append_element_json (n_inout, arr_inout, alloced_inout,
           dsk_json_value_new_object (DSK_N_ELEMENTS (members), members));
}
static void
add_user   (unsigned *n_inout,
            DskJsonValue ***arr_inout,
            unsigned *alloced_inout,
            int       px,
            int       py,
            dsk_boolean is_self)
{
  DskJsonMember members[5];
  members[0].name = "x";
  members[0].value = dsk_json_value_new_number (px);
  members[1].name = "y";
  members[1].value = dsk_json_value_new_number (py);
  members[2].name = "radius";
  members[2].value = dsk_json_value_new_number (TILE_SIZE * 3 / 8);
  members[3].name = "color";
  members[3].value = dsk_json_value_new_string (7, is_self ? "#33ff33" : "#11dd11");
  members[4].name = "type";
  members[4].value = dsk_json_value_new_string (6, "circle");

  append_element_json (n_inout, arr_inout, alloced_inout,
           dsk_json_value_new_object (DSK_N_ELEMENTS (members), members));
}
static void
add_enemy  (unsigned *n_inout,
            DskJsonValue ***arr_inout,
            unsigned *alloced_inout,
            int       px,
            int       py)
{
  DskJsonMember members[5];
  members[0].name = "x";
  members[0].value = dsk_json_value_new_number (px);
  members[1].name = "y";
  members[1].value = dsk_json_value_new_number (py);
  members[2].name = "radius";
  members[2].value = dsk_json_value_new_number (TILE_SIZE * 3 / 8);
  members[3].name = "color";
  members[3].value = dsk_json_value_new_string (7, "#ff3333");
  members[4].name = "type";
  members[4].value = dsk_json_value_new_string (6, "circle");

  append_element_json (n_inout, arr_inout, alloced_inout,
           dsk_json_value_new_object (DSK_N_ELEMENTS (members), members));
}
static void
add_generator (unsigned *n_inout,
               DskJsonValue ***arr_inout,
               unsigned *alloced_inout,
               int       px,
               int       py,
               unsigned  update_number)
{
  DskJsonMember members[6];
  static const char *colors[] = { "#ffffff", "#ff0000", "#00ff00", "#2222ff", "#ff00ff", "#00ffff", "#ffff00" };
  members[0].name = "x";
  members[0].value = dsk_json_value_new_number (px - TILE_SIZE * 7 / 8);
  members[1].name = "y";
  members[1].value = dsk_json_value_new_number (py - TILE_SIZE * 7 / 8);
  members[2].name = "width";
  members[2].value = dsk_json_value_new_number (TILE_SIZE * 7 / 4);
  members[3].name = "height";
  members[3].value = dsk_json_value_new_number (TILE_SIZE * 7 / 4);
  members[4].name = "color";
  members[4].value = dsk_json_value_new_string (7, (char*) colors[update_number % DSK_N_ELEMENTS (colors)]);
  members[5].name = "type";
  members[5].value = dsk_json_value_new_string (10, "hollow_box");

  append_element_json (n_inout, arr_inout, alloced_inout,
           dsk_json_value_new_object (DSK_N_ELEMENTS (members), members));
}

static DskJsonValue *
create_user_update (User *user)
{
  Game *game = user->game;

  /* width/height in various units, rounded up */
  unsigned tile_width = (user->width + TILE_SIZE - 1) / TILE_SIZE;
  unsigned tile_height = (user->height + TILE_SIZE - 1) / TILE_SIZE;
  unsigned cell_width = (tile_width + CELL_SIZE - 1) / CELL_SIZE;
  unsigned cell_height = (tile_height + CELL_SIZE - 1) / CELL_SIZE;

  /* left/upper corner, rounded down */
  int min_tile_x = user->x - (tile_width+1) / 2;
  int min_tile_y = user->y - (tile_height+1) / 2;
  int min_cell_x = int_div (min_tile_x, CELL_SIZE);
  int min_cell_y = int_div (min_tile_y, CELL_SIZE);

  unsigned alloced = 16;
  DskJsonValue **elements = dsk_malloc (sizeof (DskJsonValue *) * alloced);
  unsigned n_elements = 0;

  unsigned x, y;

  for (x = 0; x < cell_width; x++)
    for (y = 0; y < cell_height; y++)
      {
        int ucx = x + min_cell_x;               /* un-wrapped x, y */
        int ucy = y + min_cell_y;
        int px = (ucx * CELL_SIZE - user->x) * TILE_SIZE + user->width / 2 - TILE_SIZE / 2;
        int py = (ucy * CELL_SIZE - user->y) * TILE_SIZE + user->height / 2 - TILE_SIZE / 2;
        unsigned cx, cy;
        Cell *cell;

        /* deal with wrapping (or not) */
        if (ucx < 0)
          {
            if (!game->wrap)
              continue;
            cx = ucx + game->universe_width;
          }
        else if ((unsigned) ucx >= game->universe_width)
          {
            cx = ucx;
            if (game->wrap)
              cx -= game->universe_width;
          }
        else
          cx = ucx;
        if (ucy < 0)
          {
            if (!game->wrap)
              continue;
            cy = ucy + game->universe_height;
          }
        else if ((unsigned) ucy >= game->universe_height)
          {
            cy = ucy;
            if (game->wrap)
              cy -= game->universe_height;
          }
        else
          cy = ucy;

        /* render walls */
        if (cy < game->universe_height && cx <= game->universe_width
            && game->v_walls[cx + cy * game->universe_width])
          {
            /* render vertical wall */
            add_wall (&n_elements, &elements, &alloced,
                      px, py, TILE_SIZE, TILE_SIZE * CELL_SIZE);
          }
        if (cy <= game->universe_height && cx < game->universe_width
            && game->h_walls[cx + cy * game->universe_width])
          {
            /* render horizontal wall */
            add_wall (&n_elements, &elements, &alloced,
                      px, py, TILE_SIZE * CELL_SIZE, TILE_SIZE);
          }
        if (cx >= game->universe_width || cy >= game->universe_height)
          continue;

        cell = game->cells + (game->universe_width * cy + cx);

        /* render bullets */
        Bullet *bullet;
        for (bullet = cell->bullets; bullet; bullet = bullet->next_in_cell)
          {
            int bx = px + (bullet->x - cx * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            int by = py + (bullet->y - cy * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            add_bullet (&n_elements, &elements, &alloced,
                        bx, by);
          }

        /* render dudes */
        User *guser;
        for (guser = cell->users; guser; guser = guser->next_in_cell)
          {
            int bx = px + (guser->x - cx * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            int by = py + (guser->y - cy * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            add_user (&n_elements, &elements, &alloced,
                      bx, by, user == guser);
          }

        /* render bad guys */
        Enemy *enemy;
        for (enemy = cell->enemies; enemy; enemy = enemy->next_in_cell)
          {
            int bx = px + (enemy->x - cx * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            int by = py + (enemy->y - cy * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            add_enemy (&n_elements, &elements, &alloced, bx, by);
          }

        /* render generators */
        if (cell->generator)
          {
            int bx = px + (cell->generator->x - cx * CELL_SIZE) * TILE_SIZE + TILE_SIZE;
            int by = py + (cell->generator->y - cy * CELL_SIZE) * TILE_SIZE + TILE_SIZE;
            add_generator (&n_elements, &elements, &alloced, bx, by, user->game->latest_update);
          }
      }
  DskJsonValue *rv;
  rv = dsk_json_value_new_array (n_elements, elements);
  dsk_free (elements);
  return rv;
}

/* --- bookkeeping functions --- */
static Game *
find_game (const char *name)
{
  Game *game;
  for (game = all_games; game; game = game->next_game)
    if (strcmp (game->name, name) == 0)
      return game;
  return NULL;
}
static User *
find_user (const char *name)
{
  Game *game;
  for (game = all_games; game; game = game->next_game)
    {
      User *user;
      for (user = game->users; user; user = user->next_in_game)
        if (strcmp (user->name, name) == 0)
          return user;
    }
  return NULL;
}

/* --- CGI handlers --- */
static void
handle_main_page (DskHttpServerRequest *request)
{
  DskHttpServerResponseOptions options = DSK_HTTP_SERVER_RESPONSE_OPTIONS_DEFAULT;
  options.source_filename = "../html/snipez.html";
  options.content_type = "text/html/UTF-8";
  dsk_http_server_request_respond (request, &options);
}

static void
respond_take_json (DskHttpServerRequest *request,
                   DskJsonValue         *value)
{
  DskBuffer buffer = DSK_BUFFER_STATIC_INIT;
  DskHttpServerResponseOptions options = DSK_HTTP_SERVER_RESPONSE_OPTIONS_DEFAULT;
  dsk_json_value_to_buffer (value, -1, &buffer);
  options.source_buffer = &buffer;
  options.content_type = "application/json";
  dsk_http_server_request_respond (request, &options);
  dsk_json_value_free (value);
}

static void
handle_get_games_list (DskHttpServerRequest *request)
{
  unsigned n_games = 0;
  Game *game;
  DskJsonValue **game_info, **at;
  for (game = all_games; game; game = game->next_game)
    n_games++;
  game_info = dsk_malloc (sizeof (DskJsonValue *) * n_games);

  at = game_info;
  for (game = all_games; game; game = game->next_game, at++)
    {
      User *player;
      unsigned n_players = 0;
      DskJsonValue **pat;
      DskJsonMember members[2] = {
        { "name", dsk_json_value_new_string (strlen (game->name), game->name) },
        { "players", NULL },
      };
      DskJsonValue **players;
      player = game->users;
      for (player = game->users; player; player = player->next_in_game)
        n_players++;
      players = dsk_malloc (sizeof (DskJsonValue *) * n_players);
      pat = players;
      for (player = game->users; player; player = player->next_in_game)
        *pat++ = dsk_json_value_new_string (strlen (player->name), player->name);
      members[1].value = dsk_json_value_new_array (n_players, players);
      dsk_free (players);
      *at++ = dsk_json_value_new_object (DSK_N_ELEMENTS (members), members);
    }

  respond_take_json (request, dsk_json_value_new_array (n_games, game_info));
  dsk_free (game_info);
}


static void
handle_join_existing_game (DskHttpServerRequest *request)
{
  DskCgiVariable *game_var = dsk_http_server_request_lookup_cgi (request, "game");
  DskCgiVariable *user_var = dsk_http_server_request_lookup_cgi (request, "user");
  char buf[512];
  Game *game, *found_game = NULL;
  User *user;
  DskJsonValue *state_json;
  unsigned width, height;
  if (game_var == NULL)
    {
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, "missing game=");
      return;
    }
  if (user_var == NULL)
    {
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, "missing user=");
      return;
    }
  game = find_game (game_var->value);
  if (game == NULL)
    {
      snprintf (buf, sizeof (buf), "game %s not found", game_var->value);
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, buf);
      return;
    }
  user = find_user (user_var->value);
  if (user != NULL)
    {
      snprintf (buf, sizeof (buf), "user %s already found in %s", user->name, user->game->name);
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, buf);
      return;
    }

  width = 400;
  height = 400;
  user = create_user (found_game, user_var->value, width, height);
  state_json = create_user_update (user);
  respond_take_json (request, state_json);
}

static void
handle_create_new_game (DskHttpServerRequest *request)
{
  DskCgiVariable *game_var = dsk_http_server_request_lookup_cgi (request, "game");
  DskCgiVariable *user_var = dsk_http_server_request_lookup_cgi (request, "user");
  char buf[512];
  Game *game;
  User *user;
  DskJsonValue *state_json;
  unsigned width, height;
  if (game_var == NULL)
    {
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, "missing game=");
      return;
    }
  if (user_var == NULL)
    {
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, "missing user=");
      return;
    }
  game = find_game (game_var->value);
  if (game != NULL)
    {
      snprintf (buf, sizeof (buf), "game %s already exists", game_var->value);
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, buf);
      return;
    }
  user = find_user (user_var->value);
  if (user != NULL)
    {
      snprintf (buf, sizeof (buf), "user %s already found in %s", user->name, user->game->name);
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, buf);
      return;
    }

  game = create_game (game_var->value);
  width = 400;
  height = 400;
  user = create_user (game, user_var->value, width, height);
  state_json = create_user_update (user);
  respond_take_json (request, state_json);
}

static void
handle_update_game (DskHttpServerRequest *request)
{
  DskCgiVariable *user_var = dsk_http_server_request_lookup_cgi (request, "user");
  DskCgiVariable *actions_var = dsk_http_server_request_lookup_cgi (request, "actions");
  User *user = find_user (user_var->value);
  char buf[512];
  if (user == NULL)
    {
      snprintf (buf, sizeof (buf), "user %s not found", user_var->value);
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, buf);
      return;
    }
  if (user->last_update == user->game->latest_update)
    {
      /* wait for next frame */
      PendingUpdate *pu = dsk_malloc (sizeof (PendingUpdate));
      pu->user = user;
      pu->request = request;
      pu->next = user->game->pending_updates;
      user->game->pending_updates = pu;
    }
  else
    {
      DskJsonValue *state_json = create_user_update (user);
      respond_take_json (request, state_json);
    }
}

/* --- main program --- */
static struct {
  const char *pattern;
  void (*handler) (DskHttpServerRequest *request);
} handlers[] = {
  { "/", handle_main_page },
  { "/games(\\?.*)", handle_get_games_list },
  { "/join\\?.*", handle_join_existing_game },
  { "/newgame\\?.*", handle_create_new_game },
  { "/update\\?.*", handle_update_game },
};

int main(int argc, char **argv)
{
  unsigned port = 0;
  DskHttpServer *server;

  dsk_cmdline_init ("snipez server", "Run a snipez server", NULL, 0);
  dsk_cmdline_add_uint ("port", "Port Number",
                        "PORT", DSK_CMDLINE_MANDATORY, &port);
  dsk_cmdline_process_args (&argc, &argv);

  server = dsk_http_server_new ();
  for (i = 0; i < DSK_N_ELEMENTS (handlers); i++)
    {
      dsk_http_server_match_save (server);
      dsk_http_server_add_match (server, DSK_HTTP_SERVER_MATCH_PATH,
                                 handlers[i].pattern);
      dsk_http_server_register_cgi_handler (server,
                                            (DskHttpServerCgiFunc) handlers[i].handler,
                                            NULL, NULL);
      dsk_http_server_match_restore (server);
    }

  return dsk_main_run ();
}
