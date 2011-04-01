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

typedef struct _User User;
struct _User
{
  char *name;
  unsigned x, y;

  unsigned last_seen_time;
  Game *game;
  User *next_in_game, *prev_in_game;
  int move_x, move_y;

  /* if you connect and you already have gotten the latest
     screen, we make you wait for the next update. */
  unsigned last_update;
};

typedef struct _Enemy Enemy;
struct _Enemy
{
  unsigned x, y;
  Enemy *prev, *next;
};

typedef struct _Bullet Bullet;
struct _Bullet
{
  unsigned x, y;
  int delta_x, delta_y;
  Bullet *next_in_cell, *prev_in_cell;;
  Bullet *next_global;
};

struct _Cell
{
  Bullet *bullets;
  Enemy *enemies;
};

typedef struct _Game Game;
struct _Game
{
  char *name;
  Game *next_game;

  unsigned cell_size;
  unsigned universe_width, universe_height;     // in cells
  uint8_t *h_walls;             /* universe_height+1 x universe_width */
  uint8_t *v_walls;             /* universe_height   x universe_width+1 */

  User *users;

  Bullet **cells;               /* universe_height x universe_width */
  Bullet *all_bullets;

  unsigned latest_update;
};
static Game *all_games;


/* --- Creating a new game --- */
static Game *
create_game (const char *name)
{
  Game *game = dsk_malloc (sizeof (Game));
  ...
}

/* --- Creating a user in a game --- */
static User *
create_user (Game *game, const char *name)
{
  User *user = dsk_malloc (sizeof (User));
  user->name = dsk_strdup (name);
  user->x = ...;
  user->y = ...;
  user->last_seen_time = dsk_dispatch_default ()->last_dispatch_secs;
  user->game = game;
  user->next_in_game = game->users;
  user->prev_in_cell = NULL;
  game->users = user;
  user->move_x = user->move_y = 0;
  user->last_update = (unsigned)(-1);
  return user;
}

/* --- bookkeeping functions --- */
static Game *
find_game (const char *name)
{
  Game *game;
  for (game = all_games; game; game = game->next_game)
    if (strcmp (game->name, game_var->value) == 0)
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
      for (user = game->users; user; user = user->next)
        if (strcmp (user->name, user_var->value) == 0)
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
  DskJsonValue **game_info;
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
        { "name", dsk_json_value_new_string (game->name) },
        { "players", NULL },
      };
      player = game->users;
      for (player = game->users; player; player = player->next_in_game)
        n_players++;
      players = dsk_malloc (sizeof (DskJsonValue *) * n_players);
      pat = players;
      for (player = game->users; player; player = player->next_in_game)
        *pat++ = dsk_json_value_new_string (player->name);
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
      snprintf (buf, sizeof (buf), "game %s not found", game_var->name);
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

  user = create_user (found_game, user_var->value);
  state_json = create_user_update (user);
  respond_take_json (request, state_json);
}

static void
handle_create_new_game (DskHttpServerRequest *request)
{
  DskCgiVariable *game_var = dsk_http_server_request_lookup_cgi (request, "game");
  DskCgiVariable *user_var = dsk_http_server_request_lookup_cgi (request, "user");
  char buf[512];
  Game *game, *found_game = NULL;
  User *user;
  DskJsonValue *state_json;
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
      snprintf (buf, sizeof (buf), "game %s already exists", game_var->name);
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
  user = create_user (game, user_var->value);
  state_json = create_user_update (user);
  respond_take_json (request, state_json);
}

static void
handle_update_game (DskHttpServerRequest *request)
{
  DskCgiVariable *user_var = dsk_http_server_request_lookup_cgi (request, "user");
  DskCgiVariable *actions_var = dsk_http_server_request_lookup_cgi (request, "actions");
  user = find_user (user_var->value);
  if (user == NULL)
    {
      snprintf (buf, sizeof (buf), "user %s not found", user_var->value);
      dsk_http_server_request_respond_error (request, DSK_HTTP_STATUS_BAD_REQUEST, buf);
      return;
    }
  if (user->last_update == user->game->latest_update)
    {
      /* wait for next frame */
      ...
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
