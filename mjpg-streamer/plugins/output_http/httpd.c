/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 busybox-project (base64 function)                    #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "httpd.h"
#include "../../mjpg_streamer.h"
#include "../../utils.h"
#include "../output.h"
#include "../input.h"

static globals *pglobal;
int  sd, port;
char *credentials, *www_folder;

/******************************************************************************
Description.: initializes the iobuffer structure properly
Input Value.: pointer to already allocated iobuffer
Return Value: iobuf
******************************************************************************/
void init_iobuffer(iobuffer *iobuf) {
  memset(iobuf->buffer, 0, sizeof(iobuf->buffer));
  iobuf->level = 0;
}

/******************************************************************************
Description.: initializes the request structure properly
Input Value.: pointer to already allocated req
Return Value: req
******************************************************************************/
void init_request(request *req) {
  req->type        = A_UNKNOWN;
  req->parameter   = NULL;
  req->client      = NULL;
  req->credentials = NULL;
}

/******************************************************************************
Description.: If strings were assigned to the different members free them
              This will fail if strings are static, so always use strdup().
Input Value.: req: pointer to request structure
Return Value: -
******************************************************************************/
void free_request(request *req) {
  if ( req->parameter != NULL ) free(req->parameter);
  if ( req->client != NULL ) free(req->client);
  if ( req->credentials != NULL ) free(req->credentials);
}

/******************************************************************************
Description.: read with timeout, implemented without using signals
              tries to read len bytes and returns if enough bytes were read
              or the timeout was triggered. In case of timeout the return
              value may differ from the requested bytes "len".
Input Value.: * fd.....: fildescriptor to read from
              * iobuf..: iobuffer that allows to use this functions from multiple
                         threads because the complete context is the iobuffer.
              * buffer.: The buffer to store values at, will be set to zero
                         before storing values.
              * len....: the length of buffer
              * timeout: seconds to wait for an answer
Return Value: * buffer.: will become filled with bytes read
              * iobuf..: May get altered to save the context for future calls.
              * func().: bytes copied to buffer or -1 in case of error
******************************************************************************/
int _read(int fd, iobuffer *iobuf, void *buffer, size_t len, int timeout) {
  int copied=0, rc, i;
  fd_set fds;
  struct timeval tv;

  memset(buffer, 0, len);

  while ( (copied < len) ) {
    i = MIN(iobuf->level, len-copied);
    memcpy(buffer+copied, iobuf->buffer+IO_BUFFER-iobuf->level, i);

    iobuf->level -= i;
    copied += i;
    if ( copied >= len )
      return copied;

    /* select will return in case of timeout or new data arrived */
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    if ( (rc = select(fd+1, &fds, NULL, NULL, &tv)) <= 0 ) {
      if ( rc < 0)
        exit(EXIT_FAILURE);

      /* this must be a timeout */
      return copied;
    }

    init_iobuffer(iobuf);

    /* there should be at least one byte, because select signalled it */
    if ( (iobuf->level = read(fd, &iobuf->buffer, IO_BUFFER)) < 0 ) {
      /* an error occured */
      return -1;
    }

    /* align data to the end of the buffer if less than IO_BUFFER bytes were read */
    memmove(iobuf->buffer+(IO_BUFFER-iobuf->level), iobuf->buffer, iobuf->level);
  }

  return 0;
}

/******************************************************************************
Description.: Read a single line from the provided fildescriptor.
              This funtion will return under two conditions:
              * line end was reached
              * timeout occured
Input Value.: * fd.....: fildescriptor to read from
              * iobuf..: iobuffer that allows to use this functions from multiple
                         threads because the complete context is the iobuffer.
              * buffer.: The buffer to store values at, will be set to zero
                         before storing values.
              * len....: the length of buffer
              * timeout: seconds to wait for an answer
Return Value: * buffer.: will become filled with bytes read
              * iobuf..: May get altered to save the context for future calls.
              * func().: bytes copied to buffer or -1 in case of error
******************************************************************************/
/* read just a single line or timeout */
int _readline(int fd, iobuffer *iobuf, void *buffer, size_t len, int timeout) {
  char c='\0', *out=buffer;
  int i;

  memset(buffer, 0, len);

  for ( i=0; i<len && c != '\n'; i++ ) {
    if ( _read(fd, iobuf, &c, 1, timeout) <= 0 ) {
      /* timeout or error occured */
      return -1;
    }
    *out++ = c;
  }

  return i;
}

/******************************************************************************
Description.: Decodes the data and stores the result to the same buffer.
              The buffer will be large enough, because base64 requires more
              space then plain text.
Hints.......: taken from busybox, but it is GPL code
Input Value.: base64 encoded data
Return Value: plain decoded data
******************************************************************************/
void decodeBase64(char *data) {
  const unsigned char *in = (const unsigned char *)data;
  /* The decoded size will be at most 3/4 the size of the encoded */
  unsigned ch = 0;
  int i = 0;

  while (*in) {
    int t = *in++;

    if (t >= '0' && t <= '9')
      t = t - '0' + 52;
    else if (t >= 'A' && t <= 'Z')
      t = t - 'A';
    else if (t >= 'a' && t <= 'z')
      t = t - 'a' + 26;
    else if (t == '+')
      t = 62;
    else if (t == '/')
      t = 63;
    else if (t == '=')
      t = 0;
    else
      continue;

    ch = (ch << 6) | t;
    i++;
    if (i == 4) {
      *data++ = (char) (ch >> 16);
      *data++ = (char) (ch >> 8);
      *data++ = (char) ch;
      i = 0;
    }
  }
  *data = '\0';
}

/******************************************************************************
Description.: Send a complete HTTP response and a single JPG-frame.
Input Value.: fildescriptor fd to send the answer to
Return Value: -
******************************************************************************/
void send_snapshot(int fd) {
  unsigned char *frame=NULL;
  int frame_size=0;
  char buffer[256] = {0};

  if ( (frame = (unsigned char *)malloc(MAX_FRAME_SIZE)) == NULL ) {
    fprintf(stderr, "not enough memory\n");
    exit(EXIT_FAILURE);
  }

  /* wait for a fresh frame */
  pthread_cond_wait(&pglobal->db_update, &pglobal->db);

  /* read buffer */
  frame_size = pglobal->size;
  memcpy(frame, pglobal->buf, frame_size);
  DBG("got frame (size: %d kB)\n", frame_size/1024);

  pthread_mutex_unlock( &pglobal->db );

  /* write the response */
  sprintf(buffer, "HTTP/1.0 200 OK\r\n" \
                  "Connection: close\r\n" \
                  "Server: MJPG-Streamer\r\n" \
                  "Content-type: image/jpeg\r\n" \
                  "\r\n");

  /* send header and image now */
  if( write(fd, buffer, strlen(buffer)) < 0 ) {
    free(frame);
    return;
  }
  write(fd, frame, frame_size);

  free(frame);
}

/******************************************************************************
Description.: Send a complete HTTP response and a stream of JPG-frames.
Input Value.: fildescriptor fd to send the answer to
Return Value: -
******************************************************************************/
void send_stream(int fd) {
  unsigned char *frame=NULL;
  int frame_size=0;
  char buffer[256] = {0};

  if ( (frame = (unsigned char *)malloc(MAX_FRAME_SIZE)) == NULL ) {
    fprintf(stderr, "not enough memory\n");
    exit(EXIT_FAILURE);
  }

  sprintf(buffer, "HTTP/1.0 200 OK\r\n" \
                  "Server: MJPG-Streamer\r\n" \
                  "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n" \
                  "\r\n" \
                  "--" BOUNDARY "\n");

  if ( write(fd, buffer, strlen(buffer)) < 0 ) {
    free(frame);
    return;
  }

  while ( !pglobal->stop ) {

    /* wait for fresh frames */
    pthread_cond_wait(&pglobal->db_update, &pglobal->db);

    /* read buffer */
    frame_size = pglobal->size;
    memcpy(frame, pglobal->buf, frame_size);
    DBG("got frame (size: %d kB)\n", frame_size/1024);

    pthread_mutex_unlock( &pglobal->db );

    sprintf(buffer, "Content-type: image/jpeg\n\n");
    if ( write(fd, buffer, strlen(buffer)) < 0 ) break;

    if( write(fd, frame, frame_size) < 0 ) break;

    sprintf(buffer, "\n--" BOUNDARY "\n");
    if ( write(fd, buffer, strlen(buffer)) < 0 ) break;
  }

  free(frame);
}

/******************************************************************************
Description.: Send error messages and headers.
Input Value.: * fd.....: is the filedescriptor to send the message to
              * which..: HTTP error code, most popular is 404
              * message: append this string to the displayed response
Return Value: -
******************************************************************************/
void send_error(int fd, int which, char *message) {
  char buffer[256] = {0};

  if ( which == 401 ) {
    sprintf(buffer, "HTTP/1.0 401 Unauthorized\r\n" \
                    "Content-type: text/plain\r\n" \
                    "Connection: close\r\n" \
                    "Server: MJPG-Streamer\r\n" \
                    "WWW-Authenticate: Basic realm=\"MJPG-Streamer\"\r\n" \
                    "\r\n" \
                    "401: Not Authenticated!\r\n" \
                    "%s", message);
  } if ( which == 404 ) {
    sprintf(buffer, "HTTP/1.0 404 Not Found\r\n" \
                    "Content-type: text/plain\r\n" \
                    "Connection: close\r\n" \
                    "Server: MJPG-Streamer\r\n" \
                    "\r\n" \
                    "404: Not Found!\r\n" \
                    "%s", message);
  } else {
    sprintf(buffer, "HTTP/1.0 501 Not Implemented\r\n" \
                    "Content-type: text/plain\r\n" \
                    "Connection: close\r\n" \
                    "Server: MJPG-Streamer\r\n" \
                    "\r\n" \
                    "501: Not Implemented!\r\n" \
                    "%s", message);
  }

  write(fd, buffer, strlen(buffer));
}

/******************************************************************************
Description.: Send HTTP header and copy the content of a file. To keep things
              simple, just a single folder gets searched for the file. Just
              files with known extension and supported mimetype get served.
              If no parameter was given, the file "index.html" will be copied.
Input Value.: * fd.......: filedescriptor to send data to
              * parameter: string that consists of the filename
Return Value: -
******************************************************************************/
void send_file(int fd, char *parameter) {
  char buffer[256] = {0};
  char *extension, *mimetype=NULL;
  int i, lfd;

  /* in case no parameter was given */
  if ( parameter == NULL || strlen(parameter) == 0 )
    parameter = "index.html";

  /* find file-extension */
  if ( (extension = strstr(parameter, ".")) == NULL ) {
    send_error(fd, 404, "No file extension found");
    return;
  }

  /* determine mime-type */
  for ( i=0; i < LENGTH_OF(mimetypes); i++ ) {
    if ( strcmp(mimetypes[i].dot_extension, extension) == 0 ) {
      mimetype = (char *)mimetypes[i].mimetype;
      break;
    }
  }

  /* in case of unknown mimetype or extension leave */
  if ( mimetype == NULL ) {
    send_error(fd, 404, "MIME-TYPE not known");
    return;
  }

  /* now filename, mimetype and extension are known */
  DBG("trying to serve file \"%s\", extension: \"%s\" mime: \"%s\"\n", parameter, extension, mimetype);

  /* build the absolute path to the file */
  strncat(buffer, www_folder, sizeof(buffer)-1);
  strncat(buffer, parameter, sizeof(buffer)-strlen(buffer)-1);

  /* try to open that file */
  if ( (lfd = open(buffer, O_RDONLY)) < 0 ) {
    DBG("file %s not accessible\n", buffer);
    send_error(fd, 404, "Could not open file");
    return;
  }
  DBG("opened file: %s\n", buffer);

  /* prepare HTTP header */
  sprintf(buffer, "HTTP/1.0 200 OK\r\n" \
                  "Content-type: %s\r\n" \
                  "Connection: close\r\n" \
                  "Server: MJPG-Streamer\r\n" \
                  "\r\n", mimetype);
  i = strlen(buffer);

  /* first transmit HTTP-header, afterwards transmit content of file */
  do {
    if ( write(fd, buffer, i) < 0 ) {
      close(lfd);
      return;
    }
  } while ( (i=read(lfd, buffer, sizeof(buffer))) > 0 );

  /* close file, job done */
  close(lfd);
}

/******************************************************************************
Description.: Perform a command specified by parameter. Send response to fd.
Input Value.: * fd.......: filedescriptor to send HTTP response to.
              * parameter: specifies the command as string.
Return Value: -
******************************************************************************/
void command(int fd, char *parameter) {
  char buffer[256] = {0};
  int i, res=-100;

  /* sanity check for parameter */
  if ( parameter == NULL || strlen(parameter) > 50 || strlen(parameter) == 0 ) {
    sprintf(buffer, "HTTP/1.0 200 OK\r\n" \
                    "Content-type: text/plain\r\n" \
                    "Connection: close\r\n" \
                    "Server: MJPG-Streamer\r\n" \
                    "\r\n" \
                    "ERROR: parameter length is wrong");

    write(fd, buffer, strlen(buffer));
    return;
  }

  /*
   * determine command, try the input command-mappings first
   * this is the interface to send commands to the input plugin.
   * if the input-plugin does not implement the optional command
   * function, a short error is reported to the HTTP-client.
   */
  for ( i=0; i < LENGTH_OF(in_cmd_mapping); i++ ) {
    if ( strcmp(in_cmd_mapping[i].string, parameter) == 0 ) {

      if ( pglobal->in.cmd == NULL ) {
        send_error(fd, 501, "input plugin can not process commands");
        return;
      }

      res = pglobal->in.cmd(in_cmd_mapping[i].cmd);
      break;
    }
  }

  /* if the command is for the output plugin itself */
  for ( i=0; i < LENGTH_OF(out_cmd_mapping); i++ ) {
    if ( strcmp(out_cmd_mapping[i].string, parameter) == 0 ) {
      res = output_cmd(in_cmd_mapping[i].cmd);
      break;
    }
  }

  /* Send HTTP-response, it will send the return value of res mapped to OK or ERROR */
  sprintf(buffer, "HTTP/1.0 200 OK\r\n" \
                  "Content-type: text/plain\r\n" \
                  "Connection: close\r\n" \
                  "Server: MJPG-Streamer\r\n" \
                  "\r\n" \
                  "%s: %s", (res==0)?"OK":"ERROR", parameter);

  write(fd, buffer, strlen(buffer));
}

/******************************************************************************
Description.: Serve a connected TCP-client. This thread function is called
              for each connect of a HTTP client like a webbrowser. It determines
              if it is a valid HTTP request and dispatches between the different
              response options.
Input Value.: arg is the filedescriptor of the connected TCP socket. It must
              have been allocated so it is freeable by this thread function.
Return Value: always NULL
******************************************************************************/
/* thread for clients that connected to this server */
void *client_thread( void *arg ) {
  int fd, cnt;
  char buffer[256]={0}, *pb=buffer;
  iobuffer iobuf;
  request req;

  /* we really need the fildescriptor and it must be freeable by us */
  if (arg != NULL) {
    fd = *((int *)arg);
    free(arg);
  }
  else
    return NULL;

  /* initializes the structures */
  init_iobuffer(&iobuf);
  init_request(&req);

  /* What does the client want to receive? Read the request. */
  memset(buffer, 0, sizeof(buffer));
  if ( (cnt = _readline(fd, &iobuf, buffer, sizeof(buffer)-1, 5)) == -1 ) {
    close(fd);
    return NULL;
  }

  /* determine what to deliver */
  if ( strstr(buffer, "GET /?action=snapshot") != NULL ) {
    req.type = A_SNAPSHOT;
  }
  else if ( strstr(buffer, "GET /?action=stream") != NULL ) {
    req.type = A_STREAM;
  }
  else if ( strstr(buffer, "GET /?action=command") != NULL ) {
    int len;

    DBG("Request for command\n");
    req.type = A_COMMAND;

    /* search for second variable "command" that specifies the command */
    if ( (pb = strstr(buffer, "command=")) == NULL ) {
      DBG("no command specified, it should be passed as second variable\n");
      send_error(fd, 501, "no \"command\" specified");
      close(fd);
      return NULL;
    }

    /* req.parameter = strdup(strsep(&pb, " ")+strlen("command=")); */
    /* i prefer to validate against the character set using strspn() */
    pb += strlen("command=");
    len = MIN(MAX(strspn(pb, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_1234567890"), 0), 100);
    req.parameter = malloc(len+1);
    if ( req.parameter == NULL ) {
      exit(EXIT_FAILURE);
    }
    memset(req.parameter, 0, len+1);
    strncpy(req.parameter, pb, len);

    DBG("parameter (len: %d): \"%s\"\n", len, req.parameter);
  }
  else {
    int len;

    DBG("try to serve a file\n");
    req.type = A_FILE;

    if ( (pb = strstr(buffer, "GET /")) == NULL ) {
      DBG("HTTP request seems to be malformed\n");
      send_error(fd, 501, "Malformed HTTP request");
      close(fd);
      return NULL;
    }

    pb += strlen("GET /");
    len = MIN(MAX(strspn(pb, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._-1234567890"), 0), 100);
    req.parameter = malloc(len+1);
    if ( req.parameter == NULL ) {
      exit(EXIT_FAILURE);
    }
    memset(req.parameter, 0, len+1);
    strncpy(req.parameter, pb, len);

    DBG("parameter (len: %d): \"%s\"\n", len, req.parameter);
  }

  /*
   * parse the rest of the HTTP-request
   * the end of the request-header is a single, empty line with "\r\n"
   */
  do {
    memset(buffer, 0, sizeof(buffer));
    if ( (cnt = _readline(fd, &iobuf, buffer, sizeof(buffer)-1, 5)) == -1 ) {
      free_request(&req);
      close(fd);
      return NULL;
    }

    if ( strstr(buffer, "User-Agent: ") != NULL ) {
      req.client = strdup(buffer+strlen("User-Agent: "));
    }
    else if ( strstr(buffer, "Authorization: Basic ") != NULL ) {
      req.credentials = strdup(buffer+strlen("Authorization: Basic "));
      decodeBase64(req.credentials);
      DBG("username:password: %s\n", req.credentials);
    }

  } while( cnt > 2 && !(buffer[0] == '\r' && buffer[1] == '\n') );

  /* check for username and password if parameter -c was given */
  if ( credentials != NULL ) {
    if ( req.credentials == NULL || strcmp(credentials, req.credentials) != 0 ) {
      DBG("access denied\n");
      send_error(fd, 401, "username and password do not match to configuration");
      close(fd);
      if ( req.parameter != NULL ) free(req.parameter);
      if ( req.client != NULL ) free(req.client);
      if ( req.credentials != NULL ) free(req.credentials);
      return NULL;
    }
    DBG("access granted\n");
  }

  /* now it's time to answer */
  switch ( req.type ) {
    case A_SNAPSHOT:
      DBG("Request for snapshot\n");
      send_snapshot(fd);
      break;
    case A_STREAM:
      DBG("Request for stream\n");
      send_stream(fd);
      break;
    case A_COMMAND:
      command(fd, req.parameter);
      break;
    case A_FILE:
      if ( www_folder == NULL )
        send_error(fd, 501, "no www-folder configured");
      else
        send_file(fd, req.parameter);
      break;
    default:
      DBG("unknown request\n");
  }

  close(fd);
  free_request(&req);

  DBG("leaving HTTP client thread\n");
  return NULL;
}

/******************************************************************************
Description.: This function cleans up ressources allocated by the server_thread
Input Value.: arg is not used
Return Value: -
******************************************************************************/
void server_cleanup(void *arg) {
  static unsigned char first_run=1;

  if ( !first_run ) {
    DBG("already cleaned up ressources\n");
    return;
  }

  first_run = 0;
  DBG("cleaning up ressources allocated by server thread\n");

  close(sd);
}

/******************************************************************************
Description.: Open a TCP socket and wait for clients to connect. If clients
              connect, start a new thread for each accepted connection.
Input Value.: arg is a pointer to the globals struct
Return Value: always NULL, will only return on exit
******************************************************************************/
void *server_thread( void *arg ) {
  struct sockaddr_in addr;
  int on;
  pthread_t client;

  pglobal = arg;

  /* set cleanup handler to cleanup ressources */
  pthread_cleanup_push(server_cleanup, NULL);

  /* open socket for server */
  sd = socket(PF_INET, SOCK_STREAM, 0);
  if ( sd < 0 ) {
    fprintf(stderr, "socket failed\n");
    exit(EXIT_FAILURE);
  }

  /* ignore "socket already in use" errors */
  on = 1;
  if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
    exit(EXIT_FAILURE);
  }

  /* perhaps we will use this keep-alive feature oneday */
  /* setsockopt(sd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)); */

  /* configure server address to listen to all local IPs */
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = port; /* is already in right byteorder */
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if ( bind(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ) {
    fprintf(stderr, "bind(%d) failed\n", htons(port));
    perror("bind: ");
    exit(EXIT_FAILURE);
  }

  /* start listening on socket */
  if ( listen(sd, 10) != 0 ) {
    fprintf(stderr, "listen failed\n");
    exit(EXIT_FAILURE);
  }

  /* create a child for every client that connects */
  while ( !pglobal->stop ) {
    int *pfd = (int *)malloc(sizeof(int));

    if (pfd == NULL) {
      fprintf(stderr, "failed to allocate (a very small amount of) memory\n");
      exit(EXIT_FAILURE);
    }

    DBG("waiting for clients to connect\n");
    *pfd = accept(sd, 0, 0);

    /* start new thread that will handle this TCP connected client */
    DBG("create thread to handle client that just established a connection\n");
    pthread_create(&client, NULL, &client_thread, pfd);
    pthread_detach(client);
  }

  DBG("leaving server thread, calling cleanup function now\n");
  pthread_cleanup_pop(1);

  return NULL;
}















