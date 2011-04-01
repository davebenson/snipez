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

/* period for update timer */
#define UPDATE_PERIOD_USECS     200000

/* number of updates dying lasts for */
#define DEAD_TIME       20

/* cells moved by bullets in a cycle */
#define BULLET_SPEED    2

/* enemies move random in 1/2 of cycles */
#define ENEMY_MOVE_FRACTION    0.5

#include "../../dsk/dsk.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* XXX TODO: use better random number generator */
static unsigned random_int_range (unsigned max)
{
  return rand() % max;
}
static double random_double (void)
{
  return (double)rand () / RAND_MAX;
}

static unsigned
mod (int x, unsigned denom)
{
  int rv = x % (int) denom;
  if (rv < 0)
    rv += denom;
  return rv;
}


typedef struct _User User;
typedef struct _Enemy Enemy;
typedef struct _Bullet Bullet;
typedef struct _Generator Generator;
typedef struct _Cell Cell;
typedef struct _Game Game;

typedef struct _PendingUpdate PendingUpdate;

static DskJsonValue * create_user_update (User                 *user);
static void           respond_take_json  (DskHttpServerRequest *request,
                                          DskJsonValue         *value);

typedef enum
{
  OBJECT_TYPE_USER,
  OBJECT_TYPE_BULLET,
  OBJECT_TYPE_ENEMY
} ObjectType;
#define N_OBJECT_TYPES  3

typedef struct _Object Object;
struct _Object
{
  ObjectType type;
  Object *prev_in_game, *next_in_game;
  Object *prev_in_cell, *next_in_cell;
  Game *game;
  unsigned x, y;
};

struct _User
{
  Object base;

  char *name;
  unsigned width, height;               /* canvas width, height */

  unsigned last_seen_time;
  int move_x, move_y;

  unsigned dead_count;

  /* if you connect and you already have gotten the latest
     screen, we make you wait for the next update. */
  unsigned last_update;
};

struct _Enemy
{
  Object base;
};

struct _Bullet
{
  unsigned x, y;
  int move_x, move_y;           /* max 1 -- see bullet speed */
  Bullet *next_in_cell, *prev_in_cell;
  Bullet *next_in_game, *prev_in_game;
};

struct _Generator
{
  Game *game;
  unsigned x, y;
  double generator_prob;
  Generator *next_in_game, *prev_in_game;
};

struct _Cell
{
  Object *objects[N_OBJECT_TYPES];
  Generator *generator;
};

struct _Game
{
  char *name;
  Game *next_game;

  unsigned universe_width, universe_height;     // in cells
  uint8_t *h_walls;             /* universe_height x universe_width */
  uint8_t *v_walls;             /* universe_height x universe_width */

  Object *objects[N_OBJECT_TYPES];

  Cell *cells;               /* universe_height x universe_width */
  Generator *generators;
  dsk_boolean wrap;
  dsk_boolean diag_bullets_bounce;
  dsk_boolean bullet_kills_player;
  dsk_boolean bullet_kills_generator;

  unsigned latest_update;
  PendingUpdate *pending_updates;

  DskDispatchTimer *timer;
};
static Game *all_games;

struct _PendingUpdate
{
  User *user;
  DskHttpServerRequest *request;
  PendingUpdate *next;
};

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

static void game_update_timer_callback (Game *game);

static Game *
create_game (const char *name)
{
  Game *game = dsk_malloc (sizeof (Game));
  unsigned usize;
  unsigned i;

  game->name = dsk_strdup (name);
  game->next_game = all_games;
  all_games = game;
  game->universe_width = 32;
  game->universe_height = 32;
  usize = game->universe_height * game->universe_width;
  game->h_walls = generate_ones (usize);
  game->v_walls = generate_ones (usize);
  for (i = 0; i < N_OBJECT_TYPES; i++)
    game->objects[i] = NULL;
  game->cells = dsk_malloc0 (sizeof (Cell) * game->universe_width * game->universe_height);
  game->latest_update = 0;
  game->wrap = DSK_TRUE;
  game->diag_bullets_bounce = DSK_TRUE;
  game->bullet_kills_player = DSK_TRUE;
  game->bullet_kills_generator = DSK_TRUE;

  /* Generate with Modified Kruskals Algorithm, see 
   *    http://en.wikipedia.org/wiki/Maze_generation_algorithm
   */
  usize = game->universe_width * game->universe_height;
  TmpWall *tmp_walls = dsk_malloc (sizeof (TmpWall) * usize * 2);
  TmpSetInfo *sets = dsk_malloc (sizeof (TmpSetInfo) * usize);

  /* connect the walls together in random order */
  unsigned *scramble;
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

  /* generate generators */
  unsigned n_generators = 12 + rand () % 6;
  while (i < n_generators)
    {
      unsigned idx = random_int_range (usize);
      Cell *cell = game->cells + idx;
      if (cell->generator == NULL)
        {
          cell->generator = dsk_malloc (sizeof (Generator));
          cell->generator->game = game;
          cell->generator->x = (idx % game->universe_width) * CELL_SIZE + CELL_SIZE/2;
          cell->generator->y = (idx / game->universe_width) * CELL_SIZE + CELL_SIZE/2;
          cell->generator->generator_prob = 0.01;
          cell->generator->next_in_game = game->generators;
          cell->generator->prev_in_game = NULL;
          if (game->generators)
            game->generators->prev_in_game = cell->generator;
          game->generators = cell->generator;

          i++;
        }
    }

  game->timer = dsk_main_add_timer (0, UPDATE_PERIOD_USECS,
                                    (DskTimerFunc) game_update_timer_callback,
                                    game);
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
static Object *cell_find_object (Cell *cell,
                                 ObjectType type,
                                 unsigned x, unsigned y)
{
  Object *object;
  for (object = cell->objects[type]; object; object = object->next_in_cell)
    if (object->x == x && object->y == y)
      return object;
  return NULL;
}
static OccType
get_occupancy (Game *game, unsigned x, unsigned y, void **ptr_out)
{
  Cell *cell;
  Object *object;
  if (x >= CELL_SIZE * game->universe_width
   || y >= CELL_SIZE * game->universe_height)
    return OCC_WALL;
  cell = game->cells + (x / CELL_SIZE) + (y / CELL_SIZE) * game->universe_width;
  object = cell_find_object (cell, OBJECT_TYPE_USER, x, y);
  if (object)
    {
      *ptr_out = object;
      return OCC_USER;
    }
  if (cell->generator
      && (cell->generator->x == x || cell->generator->x + 1 == x)
      && (cell->generator->y == y || cell->generator->y + 1 == y))
    {
      *ptr_out = cell->generator;
      return OCC_GENERATOR;
    }
  object = cell_find_object (cell, OBJECT_TYPE_BULLET, x, y);
  if (object)
    {
      *ptr_out = object;
      return OCC_BULLET;
    }
  object = cell_find_object (cell, OBJECT_TYPE_ENEMY, x, y);
  if (object)
    {
      *ptr_out = object;
      return OCC_ENEMY;
    }
  return OCC_EMPTY;
}


static void
remove_object_from_cell_list (Object *object)
{
  unsigned idx = (object->x/CELL_SIZE)
               + (object->y/CELL_SIZE) * object->game->universe_width;
  Cell *cell = object->game->cells + idx;
  if (object->prev_in_cell != NULL)
    object->prev_in_cell->next_in_cell = object->next_in_cell;
  else
    cell->objects[object->type] = object->next_in_cell;
  if (object->next_in_cell != NULL)
    object->next_in_cell->prev_in_cell = object->prev_in_cell;
}

static void
add_object_to_cell_list (Object *object)
{
  unsigned idx = (object->x/CELL_SIZE)
               + (object->y/CELL_SIZE) * object->game->universe_width;
  Cell *cell = object->game->cells + idx;

  object->next_in_cell = cell->objects[object->type];
  if (object->next_in_cell)
    object->next_in_cell->prev_in_cell = object;
  object->prev_in_cell = NULL;
  cell->objects[object->type] = object;
}

static void
move_object (Object *object, unsigned x, unsigned y)
{
  remove_object_from_cell_list (object);
  object->x = x;
  object->y = y;
  add_object_to_cell_list (object);
}

static void
remove_object_from_game_list (Object *object)
{
  if (object->prev_in_game != NULL)
    object->prev_in_game->next_in_game = object->next_in_game;
  else
    object->game->objects[object->type] = object->next_in_game;
  if (object->next_in_game != NULL)
    object->next_in_game->prev_in_game = object->prev_in_game;
}

static void
add_object_to_game_list (Object *object)
{
  Game *game = object->game;
  object->next_in_game = game->objects[object->type];
  if (object->next_in_game)
    object->next_in_game->prev_in_game = object;
  object->prev_in_game = NULL;
  game->objects[object->type] = object;
}

static void
teleport_object (Object *object)
{
  Game *game = object->game;
  void *dummy;
  do
    {
      object->x = random_int_range (game->universe_width * CELL_SIZE);
      object->y = random_int_range (game->universe_height * CELL_SIZE);
    }
  while (get_occupancy (game, object->x, object->y, &dummy) != OCC_EMPTY);
}

static void
game_update_timer_callback (Game *game)
{
  /* run players */
  Object *object;

  for (object = game->objects[OBJECT_TYPE_USER]; object != NULL; )
    {
      User *user = (User *) object;
      dsk_boolean destroy_user = DSK_FALSE;
      if (user->dead_count > 0)
        {
          user->dead_count -= 1;
          if (user->dead_count)
            {
              teleport_object (&user->base);
              add_object_to_cell_list (object);
            }
          continue;
        }
      if (user->move_x || user->move_y)
        {
          int new_x = object->x + user->move_x;
          int new_y = object->y + user->move_y;
          void *obj;
          if (game->wrap)
            {
              new_x = mod (new_x, game->universe_width * CELL_SIZE);
              new_y = mod (new_y, game->universe_height * CELL_SIZE);
            }
          switch (get_occupancy (game, new_x, new_y, &obj))
            {
            case OCC_EMPTY:
              move_object (object, new_x, new_y);
              break;
            case OCC_WALL:
            case OCC_USER:
              /* move blocked harmlessly */
              break;
            case OCC_ENEMY:
              destroy_user = DSK_TRUE;
              break;
            case OCC_BULLET:
              destroy_user = DSK_TRUE;
              remove_object_from_cell_list (obj);
              remove_object_from_game_list (obj);
              break;
            case OCC_GENERATOR:
              destroy_user = DSK_TRUE;
              break;
            }
        }
      if (destroy_user)
        {
          remove_object_from_cell_list (object);
          user->dead_count = DEAD_TIME;
        }

      object = object->next_in_game;
    }

  /* update bullets, kill stuff */
  {
    unsigned bi;
    for (bi = 0; bi < BULLET_SPEED; bi++)
      {
        for (object = game->objects[OBJECT_TYPE_BULLET]; object != NULL; )
          {
            Bullet *bullet = (Bullet *) object;
            int new_x, new_y;
            dsk_boolean destroy_bullet;
  retry:
            new_x = object->x + bullet->move_x;
            new_y = object->y + bullet->move_y;
            if (game->wrap)
              {
                new_x = mod (new_x, game->universe_width * CELL_SIZE);
                new_y = mod (new_y, game->universe_height * CELL_SIZE);
              }
            destroy_bullet = DSK_TRUE;
            void *obj;
            switch (get_occupancy (game, new_x, new_y, &obj))
              {
              case OCC_EMPTY:
                move_object (object, new_x, new_y);
                destroy_bullet = DSK_FALSE;
                break;
              case OCC_WALL:
                if (bullet->move_x && bullet->move_y && game->diag_bullets_bounce)
                  {
                    void *dummy;
                    dsk_boolean xflip = get_occupancy (game, new_x, object->y, &dummy) == OCC_WALL;
                    dsk_boolean yflip = get_occupancy (game, object->x, new_y, &dummy) == OCC_WALL;
                    if (!xflip && !yflip)
                      xflip = yflip = DSK_TRUE;
                    if (xflip)
                      bullet->move_x = -bullet->move_x;
                    if (yflip)
                      bullet->move_y = -bullet->move_y;
                    goto retry;
                  }
                break;

              case OCC_USER:
                if (game->bullet_kills_player)
                  {
                    /* user dies */
                    User *user = obj;
                    remove_object_from_cell_list (obj);
                    user->dead_count = DEAD_TIME;
                  }
                break;
              case OCC_ENEMY:
                /* destroy enemy */
                remove_object_from_cell_list (obj);
                remove_object_from_game_list (obj);
                dsk_free (obj);
                break;
              case OCC_BULLET:
                /* destroy other bullet */
                remove_object_from_cell_list (obj);
                remove_object_from_game_list (obj);
                dsk_free (obj);
                break;
              case OCC_GENERATOR:
                if (game->bullet_kills_generator)
                  {
                    Generator *gen = obj;
                    Cell *cell = game->cells + (gen->x/CELL_SIZE)
                               + (gen->y/CELL_SIZE) * game->universe_width;
                    dsk_assert (cell->generator == gen);
                    cell->generator = NULL;

                    /* remove generator from list */
                    if (gen->prev_in_game)
                      gen->prev_in_game->next_in_game = gen->next_in_game;
                    else
                      game->generators = gen->next_in_game;
                    if (gen->next_in_game)
                      gen->next_in_game->prev_in_game = gen->prev_in_game;

                    cell->generator = NULL;
                    dsk_free (gen);
                  }
                break;
              }
            if (destroy_bullet)
              {
                Object *next = object->next_in_game;
                remove_object_from_cell_list (object);
                remove_object_from_game_list (object);
                dsk_free (bullet);
                object = next;
              }
            else
              object = object->next_in_game;
          }
      }
  }

  /* update enemies */
  for (object = game->objects[OBJECT_TYPE_ENEMY]; object != NULL; )
    {
      //Enemy *enemy = (Enemy *) object;
      int new_x, new_y;
      new_x = object->x;
      new_y = object->y;
      if (random_double () < ENEMY_MOVE_FRACTION)
        {
          new_x += random_int_range (3) - 1;
          new_y += random_int_range (3) - 1;
        }
      if (game->wrap)
        {
          new_x = mod (new_x, game->universe_width * CELL_SIZE);
          new_y = mod (new_y, game->universe_height * CELL_SIZE);
        }
      dsk_boolean destroy_enemy = DSK_FALSE;
      void *obj;
      switch (get_occupancy (game, new_x, new_y, &obj))
        {
        case OCC_EMPTY:
          move_object (object, new_x, new_y);
          break;
        case OCC_WALL:
          break;

        case OCC_USER:
          /* enemy kills user */
          remove_object_from_game_list (obj);
          remove_object_from_cell_list (obj);
          ((User*)obj)->dead_count = DEAD_TIME;
          break;
        case OCC_ENEMY:
          /* move suppressed */
          break;
        case OCC_BULLET:
          /* destroy bullet */
          remove_object_from_cell_list (obj);
          remove_object_from_game_list (obj);
          dsk_free (obj);
          destroy_enemy = DSK_TRUE;
          break;
        case OCC_GENERATOR:
          break;
        }
      if (destroy_enemy)
        {
          Object *next = object->next_in_game;
          remove_object_from_cell_list (object);
          remove_object_from_game_list (object);
          dsk_free (object);
          object = next;
        }
      else
        object = object->next_in_game;
    }

  /* run generators */
  Generator *gen;
  for (gen = game->generators; gen; gen = gen->next_in_game)
    {
      if (random_double () < gen->generator_prob)
        {
          /* try generating enemy */
          int positions[12][2] = { {-1,-1}, {-1,0}, {-1,1}, {-1,2},
                                   {0,2}, {1,2}, {2,2},
                                   {2,1}, {2,0}, {2,-1},
                                   {1,-1}, {0,-1} };
          unsigned p = random_int_range (12);
          int dx = positions[p][0];
          int dy = positions[p][1];
          unsigned x = gen->x + dx;
          unsigned y = gen->y + dy;
          void *dummy;
          if (get_occupancy (game, x, y, &dummy) == OCC_EMPTY)
            {
              /* create enemy */
              Enemy *enemy = dsk_malloc (sizeof (Enemy));
              enemy->base.type = OBJECT_TYPE_ENEMY;
              enemy->base.x = x;
              enemy->base.y = y;
              enemy->base.game = game;
              add_object_to_cell_list (object);
              add_object_to_game_list (object);
            }
        }
    }

  /* finish any requests that were waiting for a new frame */
  while (game->pending_updates != NULL)
    {
      DskJsonValue *state_json;
      PendingUpdate *pu = game->pending_updates;
      game->pending_updates = pu;

      state_json = create_user_update (pu->user);
      respond_take_json (pu->request, state_json);
      dsk_free (pu);
    }

  game->timer = dsk_main_add_timer (0, UPDATE_PERIOD_USECS,
                                    (DskTimerFunc) game_update_timer_callback,
                                    game);
}

/* --- Creating a user in a game --- */
static User *
create_user (Game *game, const char *name, unsigned width, unsigned height)
{
  User *user = dsk_malloc (sizeof (User));
  Cell *cell;

  user->name = dsk_strdup (name);
  user->base.type = OBJECT_TYPE_USER;
  user->base.game = game;

  /* pick random unoccupied position */
  teleport_object (&user->base);

  cell = game->cells
       + (user->base.x / CELL_SIZE)
       + (user->base.y / CELL_SIZE) * game->universe_width;

  add_object_to_game_list (&user->base);
  add_object_to_cell_list (&user->base);

  user->width = width;
  user->height = height;

  user->dead_count = 0;

  user->last_seen_time = dsk_dispatch_default ()->last_dispatch_secs;
  user->move_x = user->move_y = 0;
  user->last_update = (unsigned)(-1);
  return user;
}

/* --- rendering --- */

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
  Game *game = user->base.game;

  /* width/height in various units, rounded up */
  unsigned tile_width = (user->width + TILE_SIZE - 1) / TILE_SIZE;
  unsigned tile_height = (user->height + TILE_SIZE - 1) / TILE_SIZE;
  unsigned cell_width = (tile_width + CELL_SIZE - 1) / CELL_SIZE;
  unsigned cell_height = (tile_height + CELL_SIZE - 1) / CELL_SIZE;

  /* left/upper corner, rounded down */
  int min_tile_x = user->base.x - (tile_width+1) / 2;
  int min_tile_y = user->base.y - (tile_height+1) / 2;
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
        int px = (ucx * CELL_SIZE - user->base.x) * TILE_SIZE + user->width / 2 - TILE_SIZE / 2;
        int py = (ucy * CELL_SIZE - user->base.y) * TILE_SIZE + user->height / 2 - TILE_SIZE / 2;
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
        Object *object;
        for (object = cell->objects[OBJECT_TYPE_BULLET]; object; object = object->next_in_cell)
          {
            int bx = px + (object->x - cx * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            int by = py + (object->y - cy * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            add_bullet (&n_elements, &elements, &alloced,
                        bx, by);
          }

        /* render dudes */
        for (object = cell->objects[OBJECT_TYPE_USER]; object; object = object->next_in_cell)
          {
            int bx = px + (object->x - cx * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            int by = py + (object->y - cy * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            add_user (&n_elements, &elements, &alloced,
                      bx, by, user == (User*) object);
          }

        /* render bad guys */
        for (object = cell->objects[OBJECT_TYPE_ENEMY]; object; object = object->next_in_cell)
          {
            int bx = px + (object->x - cx * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            int by = py + (object->y - cy * CELL_SIZE) * TILE_SIZE + TILE_SIZE / 2;
            add_enemy (&n_elements, &elements, &alloced, bx, by);
          }

        /* render generators */
        if (cell->generator)
          {
            int bx = px + (cell->generator->x - cx * CELL_SIZE) * TILE_SIZE + TILE_SIZE;
            int by = py + (cell->generator->y - cy * CELL_SIZE) * TILE_SIZE + TILE_SIZE;
            add_generator (&n_elements, &elements, &alloced, bx, by, object->game->latest_update);
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
      Object *object;
      for (object = game->objects[OBJECT_TYPE_USER]; object != NULL; object = object->next_in_game)
        if (strcmp (((User*)object)->name, name) == 0)
          return (User*) object;
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
      Object *object;
      unsigned n_players = 0;
      DskJsonValue **pat;
      DskJsonMember members[2] = {
        { "name", dsk_json_value_new_string (strlen (game->name), game->name) },
        { "players", NULL },
      };
      DskJsonValue **players;
      for (object = game->objects[OBJECT_TYPE_USER]; object != NULL; object = object->next_in_game)
        n_players++;
      players = dsk_malloc (sizeof (DskJsonValue *) * n_players);
      pat = players;
      for (object = game->objects[OBJECT_TYPE_USER]; object != NULL; object = object->next_in_game)
        {
          User *player = (User *)object;
          *pat++ = dsk_json_value_new_string (strlen (player->name), player->name);
        }
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
      snprintf (buf, sizeof (buf), "user %s already found in %s", user->name, user->base.game->name);
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
      snprintf (buf, sizeof (buf), "user %s already found in %s", user->name, user->base.game->name);
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
  if (user->last_update == user->base.game->latest_update)
    {
      /* wait for next frame */
      PendingUpdate *pu = dsk_malloc (sizeof (PendingUpdate));
      pu->user = user;
      pu->request = request;
      pu->next = user->base.game->pending_updates;
      user->base.game->pending_updates = pu;
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
  unsigned i;

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
