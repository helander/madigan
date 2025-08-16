
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/time/time.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/options/options.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#define LV2_EVENT__EventPort "http://lv2plug.in/ns/ext/event#EventPort"

#define TCP_SERVER_IP   "127.0.0.1"
#define TCP_SERVER_PORT 5555

#define UI_URI "http://helander.network/lv2ui/madigan"

#define BUFFER_SIZE 2048

#define STATE_RESET 0
#define STATE_OPERATIONAL 1

typedef struct
{
    int sockfd;

    LV2_Atom_Forge forge;
    LV2_URID_Map* map;
    LV2_URID_Unmap* unmap;
    LV2UI_Request_Value* request_value;
    LV2_Log_Logger logger;
    LV2_Options_Option* options;

    LV2UI_Write_Function write;
    LV2UI_Controller controller;

    char uid[20];
    char plugin_uri[100];
    //char input_queue[100];
    int state;
    int patch_input_port;
    int midi_input_port;

    LV2_URID patch_Get;
    LV2_URID patch_Set;
    LV2_URID patch_property;
    LV2_URID patch_value;
    LV2_URID atom_eventTransfer;
    LV2_URID atom_Sequence;
    LV2_URID atom_Event;
    LV2_URID atom_Blank;
    LV2_URID atom_Object;
    LV2_URID atom_String;
    LV2_URID atom_Int;
    LV2_URID atom_Float;
    LV2_URID atom_URID;
    LV2_URID atom_Path;
    LV2_URID midi_MidiEvent;

    uint8_t forge_buf[1024];

} ThisUI;

//#include <stdio.h>
//#include <unistd.h>
//#include <stdint.h>

static uint32_t instance_counter = 0;

void get_instance_id(char* buf, size_t bufsize) {
    pid_t pid = getpid();
    uint32_t counter = __sync_fetch_and_add(&instance_counter, 1); // atomic increment
    snprintf(buf, bufsize, "%x-%x", (unsigned)pid, counter);
}


static void find_ports(const char* plugin_uri, ThisUI* ui) {
    ui->patch_input_port = -1;
    ui->midi_input_port  = -1;

    LilvWorld* world = lilv_world_new();
    lilv_world_load_all(world);

    const LilvPlugin* plugin = lilv_plugins_get_by_uri(
        lilv_world_get_all_plugins(world),
        lilv_new_uri(world, plugin_uri)
    );
    if (!plugin) {
        fprintf(stderr, "Plugin <%s> not found.\n", plugin_uri);
        lilv_world_free(world);
        return;
    }

    // Common URIs
    LilvNode* input_class   = lilv_new_uri(world, LV2_CORE__InputPort);
    LilvNode* atom_AtomPort = lilv_new_uri(world, LV2_ATOM__AtomPort);
    LilvNode* patch_Message = lilv_new_uri(world, LV2_PATCH__Message);
    LilvNode* midi_event    = lilv_new_uri(world, LV2_MIDI__MidiEvent);
    LilvNode* event_EventPort = lilv_new_uri(world, LV2_EVENT__EventPort); // legacy

    int num_ports = lilv_plugin_get_num_ports(plugin);

    for (int i = 0; i < num_ports; i++) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);

        if (!lilv_port_is_a(plugin, port, input_class))
            continue; // only input ports

        // Patch port detection (AtomPort + supports patch:Message)
        if (ui->patch_input_port == -1 &&
            lilv_port_is_a(plugin, port, atom_AtomPort) &&
            lilv_port_supports_event(plugin, port, patch_Message)) {
            ui->patch_input_port = i;
        }

        // MIDI port detection (AtomPort or legacy EventPort) + supports midi:MidiEvent
        if (ui->midi_input_port == -1 &&
            ((lilv_port_is_a(plugin, port, atom_AtomPort) ||
              lilv_port_is_a(plugin, port, event_EventPort)) &&
             lilv_port_supports_event(plugin, port, midi_event))) {
            ui->midi_input_port = i;
        }

        // If both found, stop searching
        if (ui->patch_input_port != -1 && ui->midi_input_port != -1)
            break;
    }

    lilv_node_free(input_class);
    lilv_node_free(atom_AtomPort);
    lilv_node_free(patch_Message);
    lilv_node_free(midi_event);
    lilv_node_free(event_EventPort);
    lilv_world_free(world);
}




/*
static int read_n(int fd, void *buf, size_t n) {
    size_t total = 0;
    char *p = buf;
    while (total < n) {
        ssize_t r = recv(fd, p + total, n - total, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        if (r == 0) { // EOF
            return -1;
        }
        total += (size_t)r;
    }
    return 0;
}
*/


static int recv_frame(int sock, char* buf, uint32_t bufsize) {
    uint32_t netlen;
    if (recv(sock, &netlen, 4, MSG_WAITALL) != 4) return -1;
    uint32_t len = ntohl(netlen);
    if (len > bufsize) return -1;
    if (recv(sock, buf, len, MSG_WAITALL) != (ssize_t)len) return -1;
    return len;
}



static int write_n(int fd, const void *buf, size_t n) {
    size_t total = 0;
    const char *p = buf;
    while (total < n) {
        ssize_t w = send(fd, p + total, n - total, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        total += (size_t)w;
    }
    return 0;
}

/*
static ssize_t recv_message(int fd, uint8_t **outbuf, uint32_t max_len) {
    uint32_t netlen;
    if (read_n(fd, &netlen, sizeof(netlen)) != 0) return -1;
    uint32_t len = ntohl(netlen);

    if (len == 0) {
        *outbuf = malloc(1);
        if (!*outbuf) return -1;
        (*outbuf)[0] = '\0';
        return 0;
    }

    if (len > max_len) {
        fprintf(stderr, "message too large: %u > %u\n", len, max_len);
        return -1;
    }

    uint8_t *buf = malloc(len + 1); // +1 if you want NUL for text convenience
    if (!buf) return -1;
    if (read_n(fd, buf, len) != 0) {
        free(buf);
        return -1;
    }
    buf[len] = '\0'; // safe NUL for treating as C-string if appropriate

    *outbuf = buf;
    return (ssize_t)len;
}
*/

static int send_message(int fd, const void *payload, uint32_t len) {
    uint32_t netlen = htonl(len);
    if (write_n(fd, &netlen, sizeof(netlen)) != 0) return -1;
    if (len > 0) {
        if (write_n(fd, payload, len) != 0) return -1;
    }
    return 0;
}


/*
static void dumpmem(void* start, int bytes_per_row, int rows)
{
    char* g = (char*)start;
    int n = 0;
    for (int row = 0; row < rows; row++) {
        printf("\n %04x  ", n);
        for (int k = 0; k < bytes_per_row; k++)
            printf(" %02x", g[n + k]);
        n += bytes_per_row;
    }
    fflush(stdout);
}
*/

static LV2UI_Handle instantiate(const LV2UI_Descriptor* descriptor, const char* plugin_uri, const char* bundle_path,
    LV2UI_Write_Function write_function, LV2UI_Controller controller, LV2UI_Widget* widget,
    const LV2_Feature* const* features)
{
    ThisUI* ui = (ThisUI*)calloc(1, sizeof(ThisUI));
    if (!ui) {
        return NULL;
    }
    ui->sockfd = -1;
    ui->state = STATE_RESET;
    ui->write = write_function;
    ui->controller = controller;
    sprintf(ui->plugin_uri,"%s",plugin_uri);

  get_instance_id(ui->uid,sizeof(ui->uid));
  find_ports(ui->plugin_uri, ui);


    printf("\n  instantiate Madigan UI id %s   plugin uri %s   bundle path %s   midi input port %d  patch input port %d\n", ui->uid, ui->plugin_uri, bundle_path,ui->midi_input_port,ui->patch_input_port);fflush(stdout);

    fflush(stdout);

    // Get host features
    // clang-format off
  const char* missing = lv2_features_query(
    features,
    LV2_LOG__log,         &ui->logger.log,    false,
    LV2_URID__map,        &ui->map,           true,
    LV2_URID__unmap,      &ui->unmap,           true,
    LV2_UI__requestValue, &ui->request_value, false,
    LV2_OPTIONS__options, &ui->options, false,
    NULL);
    // clang-format on


    lv2_log_logger_set_map(&ui->logger, ui->map);
    if (missing) {
        lv2_log_error(&ui->logger, "Missing feature <%s>\n", missing);
        free(ui);
        return NULL;
    }

    ui->patch_Get = ui->map->map(ui->map->handle, LV2_PATCH__Get);
    ui->patch_Set = ui->map->map(ui->map->handle, LV2_PATCH__Set);
    ui->patch_property = ui->map->map(ui->map->handle, LV2_PATCH__property);
    ui->patch_value = ui->map->map(ui->map->handle, LV2_PATCH__value);
    ui->atom_eventTransfer = ui->map->map(ui->map->handle, LV2_ATOM__eventTransfer);
    ui->atom_Sequence = ui->map->map(ui->map->handle, LV2_ATOM__Sequence);
    ui->atom_Event = ui->map->map(ui->map->handle, LV2_ATOM__Event);
    ui->atom_Blank = ui->map->map(ui->map->handle, LV2_ATOM__Blank);
    ui->atom_Object = ui->map->map(ui->map->handle, LV2_ATOM__Object);
    ui->atom_String = ui->map->map(ui->map->handle, LV2_ATOM__String);
    ui->atom_Int = ui->map->map(ui->map->handle, LV2_ATOM__Int);
    ui->atom_Float = ui->map->map(ui->map->handle, LV2_ATOM__Float);
    ui->atom_URID = ui->map->map(ui->map->handle, LV2_ATOM__URID);
    ui->atom_Path = ui->map->map(ui->map->handle, LV2_ATOM__Path);
    ui->midi_MidiEvent = ui->map->map(ui->map->handle, LV2_MIDI__MidiEvent);

/*
        if (ui->options) {
                LV2_URID ui_scale   = ui->map->map (ui->map->handle, "http://lv2plug.in/ns/extensions/ui#scaleFactor");
                                printf("\nui-scale urid %d",ui_scale);fflush(stdout);
                for (const LV2_Options_Option* o = ui->options; o->key; ++o) {
                                printf("\noption %d %s",o->key,ui->unmap->unmap(ui->unmap->handle, o->key));fflush(stdout);
                        if (o->context == LV2_OPTIONS_INSTANCE && o->key == ui_scale && o->type == ui->atom_Float) {
                                float ui_scale_value = *(const float*)o->value;
                                printf("\nui-scale option %f",ui_scale_value);fflush(stdout);
                        }
                }
        }
*/

    lv2_atom_forge_init(&ui->forge, ui->map);

    return ui;
}

static void cleanup(LV2UI_Handle handle)
{
    ThisUI* ui = (ThisUI*)handle;

    free(ui);
}

static void
an_object(ThisUI* ui, LV2_Atom_Object* obj)
{
    char message[1000];          
    sprintf(message, "source|%s||object|%s", ui->uid, ui->unmap->unmap(ui->unmap->handle, obj->body.otype));
    LV2_ATOM_OBJECT_FOREACH(obj, p)
    {
        if (p->value.type == ui->atom_Int) {
            LV2_Atom_Int* intAtom = (LV2_Atom_Int*)&p->value;
            sprintf(message + strlen(message), "key|%s|type|integer|value|%d|", ui->unmap->unmap(ui->unmap->handle, p->key), intAtom->body);
        } else if (p->value.type == ui->atom_Float) {
            LV2_Atom_Float* floatAtom = (LV2_Atom_Float*)&p->value;
            sprintf(message + strlen(message), "key|%s|type|float|value|%f|", ui->unmap->unmap(ui->unmap->handle, p->key), floatAtom->body);
        } else if (p->value.type == ui->atom_String) {
            LV2_Atom_String* stringAtom = (LV2_Atom_String*)&p->value;
            sprintf(message + strlen(message), "key|%s|type|string|value|%s|", ui->unmap->unmap(ui->unmap->handle, p->key), ((char*)stringAtom) + sizeof(LV2_Atom_String));
        } else if (p->value.type == ui->atom_Path) {
            LV2_Atom_String* pathAtom = (LV2_Atom_String*)&p->value;
            sprintf(message + strlen(message), "key|%s|type|path|value|%s|", ui->unmap->unmap(ui->unmap->handle, p->key), ((char*)pathAtom) + sizeof(LV2_Atom_String));
        } else if (p->value.type == ui->atom_URID) {
            LV2_Atom_URID* uridAtom = (LV2_Atom_URID*)&p->value;
            sprintf(message + strlen(message), "key|%s|type|uri|value|%s|", ui->unmap->unmap(ui->unmap->handle, p->key), ui->unmap->unmap(ui->unmap->handle,uridAtom->body));
        } else if (p->value.type == ui->atom_Object) {
            an_object(ui, (LV2_Atom_Object*)p);
        } else {
            printf("\n Unsupported atom type %s  size %d ", ui->unmap->unmap(ui->unmap->handle, p->value.type),p->value.size);
            fflush(stdout);
            return;
        }
    }
    printf("\nan_object:MESSAGE %s", message);fflush(stdout); 
      if (ui->sockfd != -1) {
         int status = send_message(ui->sockfd,message,strlen(message));
         if (!status) ui->state = STATE_OPERATIONAL;
      }


}

static void port_event(LV2UI_Handle handle, uint32_t port_index, uint32_t buffer_size, uint32_t format,
    const void* buffer)
{
    ThisUI* ui = (ThisUI*)handle;
    printf("\nPort event port %d format %d",port_index, format);fflush(stdout);
    if(!format) return;

    if (format != ui->atom_eventTransfer) {
        fprintf(stdout, "\nThisUI: Unexpected (not event transfer) message format %d  %s.\n",format,ui->unmap->unmap(ui->unmap->handle,format));
        fflush(stdout);
        return;
    }

    LV2_Atom* atom = (LV2_Atom*)buffer;
        fprintf(stdout, "ThisUI: Atom size %d  type  %d %s  \n",atom->size,atom->type,ui->unmap->unmap(ui->unmap->handle,atom->type));

    if (atom->type == ui->midi_MidiEvent) {
        return;
    }

    if (atom->type != ui->atom_Blank && atom->type != ui->atom_Object) {
        fprintf(stdout, "ThisUI: not an atom:Blank|Object msg. %d %s  \n",atom->type,ui->unmap->unmap(ui->unmap->handle,atom->type));
        return;
    }

    LV2_Atom_Object* obj = (LV2_Atom_Object*)atom;

    an_object(ui, obj);
}



#define MAX_PARTS 50  // maximum number of parts we will store

int split_on_delim(char *str, char *delim, char *parts[], int max_parts) {
    char *start = str;
    char *pos;
    int count = 0;

    while ((pos = strstr(start, delim)) != NULL) {
        *pos = '\0';                   // terminate current part
        parts[count++] = start;
        if (count >= max_parts) break; // prevent overflow
        start = pos + strlen(delim);               // move past delimiter
    }

    // Add the last part
    if (count < max_parts) {
        parts[count++] = start;
    }

    return count; // return the number of parts found
}

static int handle_server_message(char *message, ThisUI* ui) {

    printf("\nMessage with %d bytes received  %s", strlen(message), message);fflush(stdout);

    char *msg_type = NULL;
    char *msg_key = NULL;
    char *msg_value = NULL;

    char *props[15];
    int num_props = split_on_delim(message, "||", props, 15);
    for (int i = 0; i < num_props; i++) {
        printf("\n[%d] \"%s\"", i, props[i]);
        char *parts[2]; 
        int num_parts = split_on_delim(props[i], "|", parts, 2);
        if (num_parts == 2) {
          if (!strcmp(parts[0], "type")) {
             msg_type = parts[1];
          } else if (!strcmp(parts[0], "key")) {
             msg_key = parts[1];
          } else if (!strcmp(parts[0], "value")) {
             msg_value = parts[1];
          }
        }
    }

    printf("\nCmd %s  %s   %s", msg_type, msg_key, msg_value);
    fflush(stdout);

    if (!strcmp(msg_type,"control-input-port")) {
       float value = atof(msg_value);
       int port_index = atoi(msg_key);
       ui->write(ui->controller, port_index, sizeof(float), /*ui->ui_floatProtocol*/ 0, &value);
       return 0;
    }

    if (!strcmp(msg_type,"patch-parameter") && ui->patch_input_port >= 0) {
       LV2_Atom_Forge forge;
       uint8_t buffer[1000];
       LV2_Atom_Forge_Frame frame;

       LV2_URID parameter_key = ui->map->map(ui->map->handle, msg_key);
       lv2_atom_forge_init(&forge, ui->map);

       lv2_atom_forge_set_buffer(&forge, buffer, sizeof(buffer));
       lv2_atom_forge_object(&forge, &frame, 0, ui->patch_Set);
       lv2_atom_forge_key(&forge, ui->patch_property);
       lv2_atom_forge_urid(&forge, parameter_key);
       lv2_atom_forge_key(&forge, ui->patch_value);
       lv2_atom_forge_string(&forge, msg_value, strlen(msg_value));
       lv2_atom_forge_pop(&forge, &frame);

       ui->write(ui->controller, ui->patch_input_port, ((LV2_Atom*)buffer)->size + sizeof(LV2_Atom), ui->atom_eventTransfer, buffer);
       return 0;
    }

    if (!strcmp(msg_type,"midicc-parameter") && ui->midi_input_port >= 0) {
//       LV2_Atom_Forge forge;
       uint8_t buffer[1000];
//       LV2_Atom_Forge_Frame frame;

/*
       int channel = 0;
       uint8_t msg[3] = {
          (uint8_t)(0xB0 | (channel & 0x0F)),
          atoi(msg_key),
          atoi(msg_value)
       };
       printf("\nmididata %02x %02x %02x",msg[0],msg[1],msg[2]);

       lv2_atom_forge_init(&forge, ui->map);

       lv2_atom_forge_set_buffer(&forge, buffer, sizeof(buffer));
       lv2_atom_forge_sequence_head(&forge, &frame, 0);
       lv2_atom_forge_frame_time(&forge, 0);
       lv2_atom_forge_atom(&forge, 3, ui->midi_MidiEvent);
       lv2_atom_forge_write(&forge, msg, sizeof(msg));
       lv2_atom_forge_pop(&forge, &frame);
*/
////
//#include <stdint.h>
//#include <lv2/atom/atom.h>
//#include <lv2/midi/midi.h>
//#include <lv2/urid/urid.h>

//uint32_t make_single_midi_sequence(uint8_t* buf, LV2_URID atom_sequence_urid,
//                                   LV2_URID midi_event_urid,
//                                   uint8_t status, uint8_t d1, uint8_t d2)
//{
    // Atom:Sequence header
    LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)buffer;
    seq->atom.type = ui->atom_Sequence;
    seq->atom.size = sizeof(LV2_Atom_Event) + 3; // event header + MIDI bytes
    seq->body.unit = 0;   // frames
    seq->body.pad  = 0;

    // First (and only) event
    LV2_Atom_Event* ev = (LV2_Atom_Event*)(seq + 1);
    ev->time.frames = 0;
    ev->body.type   = ui->midi_MidiEvent;
    ev->body.size   = 3;

    // MIDI message
    uint8_t* midi = (uint8_t*)(ev + 1);
       int channel = 0;
          midi[0] = (uint8_t)(0xB0 | (channel & 0x0F));
          midi[1] = atoi(msg_key);
          midi[2] = atoi(msg_value);


//    return sizeof(LV2_Atom) + seq->atom.size; // total size in bytes
//}
////

       printf("\n%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
         buffer[0],buffer[1],buffer[2],buffer[3],buffer[4],buffer[5],buffer[6],buffer[7],buffer[8],buffer[9],buffer[10],buffer[11],buffer[12],buffer[13],buffer[14],buffer[15],buffer[16],buffer[17],buffer[18],buffer[19]);fflush(stdout);
       ui->write(ui->controller, ui->midi_input_port, lv2_atom_total_size((LV2_Atom*)buffer), ui->atom_eventTransfer, buffer);

       return 0;
    }

/*
    uint8_t obj_buf[2000];
    lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 2000);
    LV2_Atom* msg = NULL;

    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_frame_time (&ui->forge, 0);

    char *token = strtok(message, "|");

    if (strcmp(token,"port")) return 0;
    token = strtok(NULL,"|");
    uint32_t portIndex = atoi(token);
    token = strtok(NULL,"|");
    if (!strcmp(token,"control")) {
       token = strtok(NULL,"|");
       float value = atof(token);
       ui->write(ui->controller, portIndex, sizeof(float),  0, &value);
       return 0;
    }
    if (!strcmp(token,"object")) {
       token = strtok(NULL,"|");
       if (token) {
          LV2_URID object = ui->map->map(ui->map->handle, token);
          msg = (LV2_Atom*)lv2_atom_forge_object (&ui->forge, &frame, 0, object);
          token = strtok(NULL,"|");
          while (token != NULL) {
            if (!strcmp(token,"key")) {
	      token = strtok(NULL,"|");
              if (token) {
                 LV2_URID key = ui->map->map(ui->map->handle, token);
                 token = strtok(NULL,"|");
                 if (token) {
                   if (!strcmp(token,"type")) {
                      token = strtok(NULL,"|");
                      if (token) {
                        char *type = token;
                        token = strtok(NULL,"|");
                        if (token) {
                           if (!strcmp(token,"value")) {
                              token = strtok(NULL,"|");
                              if (token) {
                                 char *value = token;
                                 //printf("\nobject %d key %d type %s value %s",object,key,type,value);fflush(stdout);
                                 lv2_atom_forge_property_head (&ui->forge, key, 0);
                                 if (!strcmp(type,"integer")) {
                                    lv2_atom_forge_int (&ui->forge, atoi(value));
                                 } else if (!strcmp(type,"string")){
                                    lv2_atom_forge_string (&ui->forge, value, strlen (value));
                                 } else if (!strcmp(type,"path")){
                                    lv2_atom_forge_path (&ui->forge, value, strlen (value));
                                 } else if (!strcmp(type,"uri")){
                                    lv2_atom_forge_urid (&ui->forge, ui->map->map(ui->map->handle, value));
                                 }
                              }
                           }
                        }
                      }
                   }
                 }
              }
            }
            token = strtok(NULL, "|");
         }
       }
    }

    lv2_atom_forge_pop (&ui->forge, &frame);

    if (msg)
        ui->write(ui->controller, portIndex, lv2_atom_total_size(msg), ui->atom_eventTransfer, msg);
*/
    return 0;

}


/* Idle interface for UI. */
static int ui_idle(LV2UI_Handle handle)
{
    ThisUI* ui = (ThisUI*)handle;
    //if (ui->sockfd <= 0)
    //    return 0;
 
    if (ui->state == STATE_RESET) {
         printf("\nIDLE STATE_RESET");fflush(stdout);
       ui->sockfd = socket(AF_INET, SOCK_STREAM, 0);
       struct sockaddr_in servaddr = {0};
       servaddr.sin_family = AF_INET;
       servaddr.sin_port = htons(TCP_SERVER_PORT);
       inet_pton(AF_INET, TCP_SERVER_IP, &servaddr.sin_addr);

       if (connect(ui->sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
          perror("connect");
          return -1;
       }

         char message[200];
         sprintf(message,"source|%s||plugin|%s",ui->uid,ui->plugin_uri);
         printf("\nMESSAGE %s", message);fflush(stdout); 
         int status = send_message(ui->sockfd, message, strlen(message));
         if (!status) ui->state = STATE_OPERATIONAL;
    }

/////

    // Poll socket with select (non-blocking)
    fd_set rfds;
    struct timeval tv = {0, 0}; // no wait
    FD_ZERO(&rfds);
    FD_SET(ui->sockfd, &rfds);

    int ret = select(ui->sockfd + 1, &rfds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(ui->sockfd, &rfds)) {
        char buf[BUFFER_SIZE];
        int len = recv_frame(ui->sockfd, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            handle_server_message(buf, ui);
        }
    }
    return 0; // 0 = keep calling idle


/////

/*
    while (1) {
        uint8_t *buf;
        int len = recv_message(ui->sockfd, &buf, 1000);
        if (len < 0) {
           break;
        } else if (len > 0){
            handle_server_message((char *)buf, ui);
        }
        free(buf);
    }
   return 0;
*/
}

static int noop()
{
    return 0;
}

static const void* extension_data(const char* uri)
{
    static const LV2UI_Show_Interface show = { noop, noop };
    static const LV2UI_Idle_Interface idle = { ui_idle };

    printf("\nExtension data %s",uri);fflush(stdout);

    if (!strcmp(uri, LV2_UI__showInterface)) {
        return &show;
    }

    if (!strcmp(uri, LV2_UI__idleInterface)) {
        return &idle;
    }

    return NULL;
}

static const LV2UI_Descriptor descriptor = { UI_URI, instantiate, cleanup, port_event, extension_data };

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
