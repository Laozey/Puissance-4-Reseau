#include "../server_func.hpp"
#include "../../Puiss4/p4.hpp"
#include "../tcp.h"
#include "../tlv.hpp"
#include "../util_func.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <wait.h>

// A faire :
// - Corriger potentiel bug

int serverCore(int sockfd) {
  int rc;

  int fds[CONNEXIONS_LIMIT];

  while (1) {
    for (size_t i = 0; i < CONNEXIONS_LIMIT; i++) {
      struct sockaddr_in6 addr = {0};
      socklen_t addrlen = sizeof(addr);
      fds[i] = accept(sockfd, (struct sockaddr *)&addr, &addrlen);
      ERROR_HANDLER("accept()", fds[i]);

      printf("Connection from %s\n", str_of_sockaddr((struct sockaddr *)&addr));
    }

    pid_t pid;

    pid = fork();
    if (pid < 0) {
      ERROR_HANDLER("fork()", pid);
      ERROR_HANDLER("closeFds(fds)", closeFds(fds, CONNEXIONS_LIMIT));
    }
    else if (pid == 0) {
      ERROR_SHUTDOWN("childWork(fds)", childWork(fds)); // Manage connexions

      ERROR_SHUTDOWN("closeFds(fds)", closeFds(fds, CONNEXIONS_LIMIT));

      exit(0);
    }

    // Parent
    ERROR_HANDLER("closeFds(fds)", closeFds(fds, CONNEXIONS_LIMIT));

    while (1) {
      pid = waitpid(-1, NULL, WNOHANG);
      ERROR_HANDLER("waitpid(-1, NULL, WNOHANG)", pid);
    }
  }

  return sockfd;
}

int childWork(int *fds) {
  int rc;
  size_t i;

  Pseudo_t pseudo[2];
  Start_t start[2];

  Generic_tlv_t *tlv;

  for (i = 0; i < CONNEXIONS_LIMIT; i++) {
    rc = read_tlv(tlv, fds[i]); // Read tlv
    if (rc < 0) {
      ERROR_HANDLER("read_tlv(tlv, fds[i])", rc);
      return -1;
    }

    pseudo[i] = READ_PSEUDO(tlv->msg);
  }

  Puissance4_t game;
  gameInit(&game);

  Grid_t grid;
  grid.who = game.player;
  grid.won_draw = 0;
  grid.Grid = game.grid;

  int color = rand() % 2;

  for (i = 0; i < CONNEXIONS_LIMIT; i++) {
    start[i].Client = pseudo[i];
    start[i].Opponent = pseudo[(i + 1) % 2];
    start[i].Pcolor = color;

    rc = SEND_START(start[i], fds[i]);
    if (rc < 0) {
      ERROR_HANDLER("SEND_START(start[i], fds[i])", rc);
      return -1;
    }

    rc = SEND_GRID(grid, fds[i]);
    if (rc < 0) {
      ERROR_HANDLER("SEND_GRID(grid, fds[i])", rc);
      return -1;
    }

    color = (color + 1) % 2; // switch player si color = 0 -> 1 / = 1 -> 0
  }

  while (1) {
    rc = read_tlv(tlv, fds[game.player]);
    if (rc < 0) {
      ERROR_HANDLER("read_tlv(tlv, fds[game.player])", rc);
      return -1;
    }

    // Decrypte tlv
    rc = process_tlv(tlv, fds, &game);
    if (rc < 0) {
      ERROR_HANDLER("process_tlv(tlv, fds, &game)", rc);
      return -1;
    }

    if (rc == 1) { // Partie fini
      break;
    }
  }

  exit(0);
}

int process_tlv(Generic_tlv_t *tlv, int *fds, Puissance4_t *game) {
  int rc;

  switch (tlv->type) {
  case TYPE_MOVE: {
    rc = moveProcess(tlv, fds, game);
    if (rc < 0) {
      ERROR_HANDLER("moveProcess(tlv, fds, game)", rc);
      return -1;
    }

    break;
  }

  case TYPE_CONCEDE: {
    rc = SEND_CONCEDE(fds[(game->player + 1) % 2]);
    if (rc < 0) {
      ERROR_HANDLER("SEND_CONCEDE(fds[(game->player + 1) % 2])", rc);
      return -1;
    }

    rc = closeFds(fds, CONNEXIONS_LIMIT);
    if (rc < 0) {
      ERROR_HANDLER("closeFds(fds, CONNEXIONS_LIMIT)", rc);
      return -1;
    }

    break;
  }

  case TYPE_DISCON: {
    rc = SEND_DISCON(fds[(game->player + 1) % 2]);
    if (rc < 0) {
      ERROR_HANDLER("SEND_DISCON(fds[(game->player + 1) % 2])", rc);
      return -1;
    }

    rc = closeFds(fds, CONNEXIONS_LIMIT);
    if (rc < 0) {
      ERROR_HANDLER("closeFds(fds, CONNEXIONS_LIMIT)", rc);
      return -1;
    }

    break;
  }

  default: 
    fprintf(stderr, "Unknown tlv\n");
    break;
  }

  destroy_tlv(tlv);
  return rc;
}

int moveProcess(Generic_tlv_t *tlv, int *fds, Puissance4_t *game) {
  int rc;

  Move_t move = READ_MOVE(tlv->msg);

  int state = gameTurn(game, move);

  rc = moveProcessAux(move, state, fds, game);
  if (rc < 0) {
    ERROR_HANDLER("moveProcessAux(move, NOT_ACCEPTED, game->player, state, fds, game)", rc);
    return -1;
  }

  return 0;
}

int moveProcessAux(Move_t move, int state, int *fds, Puissance4_t *game) {
  int rc = 0;

  uint8_t who_to_send = game->player;
  Validity_t move_accepted = ACCEPTED;

  if (state == RUNNING) {
    uint8_t player = (game->player + 1) % 2;
    who_to_send = player;
  }
  else {
    move_accepted = NOT_ACCEPTED;
  }

  Moveack_t moveack;
  moveack.Accepted = move_accepted;
  moveack.Col = move;

  Grid_t grid;
  grid.Grid = game->grid;
  grid.who = who_to_send;
  grid.won_draw = state;

  rc = SEND_MOVEACK(moveack, fds[who_to_send]);
  if (rc < 0) {
    ERROR_HANDLER("SEND_MOVEACK(moveack, fds[who_to_send])", rc);
    return -1;
  }

  rc = SEND_GRID(grid, fds[who_to_send]);
  if (rc < 0) {
    ERROR_HANDLER("SEND_GRID(grid, fds[who_to_send])", rc);
    return -1;
  }

  if (state == WIN || state == DRAW) {
    rc = SEND_MOVEACK(moveack, fds[(who_to_send + 1) % 2]);
    if (rc < 0) {
      ERROR_HANDLER("SEND_MOVEACK(moveack, fds[(who_to_send + 1) % 2])", rc);
      return -1;
    }

    rc = SEND_GRID(grid, fds[(who_to_send + 1) % 2]);
    if (rc < 0) {
      ERROR_HANDLER("SEND_GRID(grid, fds[(who_to_send + 1) % 2])", rc);
      return -1;
    }
  }
}