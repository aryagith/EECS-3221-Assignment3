/*
 * alarm_cond.c
 *
 * This is an enhancement to the alarm_mutex.c program, which
 * used only a mutex to synchronize access to the shared alarm
 * list. This version adds a condition variable. The alarm
 * thread waits on this condition variable, with a timeout that
 * corresponds to the earliest timer request. If the main thread
 * enters an earlier timeout, it signals the condition variable
 * so that the alarm thread will wake up and process the earlier
 * timeout first, requeueing the later request.
 */
#include <pthread.h>
#include <time.h>
#include "errors.h"

/*
 * The "alarm" structure now contains the time_t (time since the
 * Epoch, in seconds) for each alarm, so that they can be
 * sorted. Storing the requested number of seconds would not be
 * enough, since the "alarm thread" cannot tell how long it has
 * been on the list.
 */
typedef struct alarm_tag {
    struct alarm_tag    *link;
    int                 seconds;
    time_t              time;   /* seconds from EPOCH */
    char                message[64];
    int                 id;
    int                 groupId;
} alarm_t;

// Global array to track which groups have an active display thread
#define MAX_GROUPS 256  // Maximum number of groups that can be tracked
int active_group_threads[MAX_GROUPS] = {0};  // 0 means no thread, 1 means a thread exists

pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;
alarm_t *alarm_list = NULL;
time_t current_alarm = 0;


void handle_invalid_request() {
    printf("Error: Invalid request format. Request discarded.\n");
}
void get_current_time(char *buffer, size_t size) {
    time_t now = time(NULL);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

void insert_alarm(int id, int groupId, int seconds, const char *message) {
    
    alarm_t *new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (!new_alarm) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return;
    }

    // Initialize the new alarm
    new_alarm->id = id;
    new_alarm->groupId = groupId;
    new_alarm->seconds = seconds;
    new_alarm->time = time(NULL) + seconds;  // Alarm time is seconds from now
    strncpy(new_alarm->message, message, sizeof(new_alarm->message) - 1);
    new_alarm->message[sizeof(new_alarm->message) - 1] = '\0'; // Ensure null-termination
    new_alarm->link = NULL;

    // Lock the mutex to safely modify the alarm list
    pthread_mutex_lock(&alarm_mutex);

    // Insert the new alarm in the list in sorted order by id
    if (!alarm_list || alarm_list->id > id) {
        new_alarm->link = alarm_list;
        alarm_list = new_alarm;  // If the list is empty or this alarm has a smaller id, insert at the front
    } else {
        alarm_t *current = alarm_list;
        while (current->link && current->link->id < id) {
            current = current->link;  // Traverse the list to find the right spot
        }
        new_alarm->link = current->link;
        current->link = new_alarm;  // Insert the new alarm in the correct spot
    }

    // Unlock the mutex
    pthread_mutex_unlock(&alarm_mutex);

    // Print confirmation message
    char time_buffer[64];
    get_current_time(time_buffer, sizeof(time_buffer));
    printf("Alarm(%d) Inserted by Main Thread %ld Into Alarm List at %s: Group(%d) %d %s\n",
           id, pthread_self(), time_buffer, groupId, seconds, new_alarm->message);
}


/*
 * Insert alarm entry on list, in order.
 */
void alarm_insert (alarm_t *alarm)
{
    int status;
    alarm_t **last, *next;

    /*
     * LOCKING PROTOCOL:
     * 
     * This routine requires that the caller have locked the
     * alarm_mutex!
     */
    last = &alarm_list;
    next = *last;
    while (next != NULL) {
        if (next->time >= alarm->time) {
            alarm->link = next;
            *last = alarm;
            break;
        }
        last = &next->link;
        next = next->link;
    }
    /*
     * If we reached the end of the list, insert the new alarm
     * there.  ("next" is NULL, and "last" points to the link
     * field of the last item, or to the list header.)
     */
    if (next == NULL) {
        *last = alarm;
        alarm->link = NULL;
    }
#ifdef DEBUG
    printf ("[list: ");
    for (next = alarm_list; next != NULL; next = next->link)
        printf ("%d(%d)[\"%s\"] ", next->time,
            next->time - time (NULL), next->message);
    printf ("]\n");
#endif
    /*
     * Wake the alarm thread if it is not busy (that is, if
     * current_alarm is 0, signifying that it's waiting for
     * work), or if the new alarm comes before the one on
     * which the alarm thread is waiting.
     */
    if (current_alarm == 0 || alarm->time < current_alarm) {
        current_alarm = alarm->time;
        status = pthread_cond_signal (&alarm_cond);
        if (status != 0)
            err_abort (status, "Signal cond");
    }
}

/*
 * The alarm thread's start routine.
 */
void *alarm_thread (void *arg)
{
    alarm_t *alarm;
    struct timespec cond_time;
    time_t now;
    int status, expired;
    pthread_t thread;
    /*
     * Loop forever, processing commands. The alarm thread will
     * be disintegrated when the process exits. Lock the mutex
     * at the start -- it will be unlocked during condition
     * waits, so the main thread can insert alarms.
     */
    status = pthread_mutex_lock (&alarm_mutex);
    if (status != 0)
        err_abort (status, "Lock mutex");
    while (1) {
        /*
         * If the alarm list is empty, wait until an alarm is
         * added. Setting current_alarm to 0 informs the insert
         * routine that the thread is not busy.
         */
        current_alarm = 0;
        while (alarm_list == NULL) {
            status = pthread_cond_wait (&alarm_cond, &alarm_mutex);
            if (status != 0)
                err_abort (status, "Wait on cond");
            }
        alarm = alarm_list;
        alarm_list = alarm->link;
        now = time (NULL);
        expired = 0;
        if (alarm->time > now) {
#ifdef DEBUG
            printf ("[waiting: %d(%d)\"%s\"]\n", alarm->time,
                alarm->time - time (NULL), alarm->message);
#endif
            cond_time.tv_sec = alarm->time;
            cond_time.tv_nsec = 0;
            current_alarm = alarm->time;
            while (current_alarm == alarm->time) {
                status = pthread_cond_timedwait (
                    &alarm_cond, &alarm_mutex, &cond_time);
                if (status == ETIMEDOUT) {
                    expired = 1;
                    break;
                }
                if (status != 0)
                    err_abort (status, "Cond timedwait");
            }
            if (!expired)
                alarm_insert (alarm);
        } else
            expired = 1;
        if (expired) {
            printf ("(%d) %s\n", alarm->seconds, alarm->message);
            free (alarm);
        }
    }
}


void *display_alarm_thread(void *arg) {
   int group_id = *((int *)arg);  // Extract the group_id from the argument

    while (1) {
        pthread_mutex_lock(&alarm_mutex);  // Lock the mutex before accessing the alarm list
        
        alarm_t *current = alarm_list;  // Pointer to traverse the alarm list
        
        // Traverse through the linked list of alarms
        while (current != NULL) {
            // If the alarm belongs to the specified group and the time has arrived, display it
            if (current->groupId == group_id && time(NULL) >= current->time) {
                // Print the alarm message
                char time_buffer[64];
                get_current_time(time_buffer, sizeof(time_buffer));
                printf("Alarm(%d) Printed by Display Alarm Thread %ld at %s: Group(%d) %d %s\n",
                       current->id, pthread_self(), time_buffer, current->groupId,
                       current->seconds, current->message);
                
                // Update the alarm time to trigger again after the specified seconds
                current->time = time(NULL) + current->seconds;
            }
            
            current = current->link;  // Move to the next alarm in the list
        }
        
        pthread_mutex_unlock(&alarm_mutex);  // Unlock the mutex after accessing the list
        
        sleep(1);  // Sleep for 1 second before checking again
    }
    return NULL;  // End the thread function
}
void *group_display_creation_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&alarm_mutex); // Lock the mutex to access the alarm list

        // Wait until the alarm list is updated
        pthread_cond_wait(&alarm_cond, &alarm_mutex);

        alarm_t *current = alarm_list; // Pointer to traverse the alarm list

        while (current != NULL) {
            int group_id = current->groupId;

            // If there is no active thread for this group, create one
            if (active_group_threads[group_id] == 0) {
                int *group_id_ptr = malloc(sizeof(int));
                if (!group_id_ptr) {
                    fprintf(stderr, "Error: Memory allocation failed\n");
                    pthread_mutex_unlock(&alarm_mutex);
                    exit(1);
                }
                *group_id_ptr = group_id;

                pthread_t thread;
                if (pthread_create(&thread, NULL, display_alarm_thread, (void *)group_id_ptr) != 0) {
                    fprintf(stderr, "Error: Unable to create display alarm thread for group %d\n", group_id);
                    free(group_id_ptr);
                    pthread_mutex_unlock(&alarm_mutex);
                    exit(1);
                }

                pthread_detach(thread); // Detach the thread so it doesn't need to be joined
                active_group_threads[group_id] = 1;

                // Log the creation of a new thread
                char time_buffer[64];
                get_current_time(time_buffer, sizeof(time_buffer));
                printf("Alarm Group Display Creation Thread Created New Display Alarm Thread %ld "
                       "For Alarm(%d) at %s: Group(%d) %d %s\n",
                       thread, current->id, time_buffer, group_id, current->seconds, current->message);
            } else {
                // Log the assignment of an alarm to an existing thread
                char time_buffer[64];
                get_current_time(time_buffer, sizeof(time_buffer));
                printf("Alarm Group Display Creation Thread Assigned Display Alarm Thread For Alarm(%d) "
                       "at %s: Group(%d) %d %s\n",
                       current->id, time_buffer, group_id, current->seconds, current->message);
            }

            current = current->link; // Move to the next alarm
        }

        pthread_mutex_unlock(&alarm_mutex); // Unlock the mutex after processing the list
    }
    return NULL;
}
void *group_display_removal_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&alarm_mutex); // Lock the mutex to access the alarm list

        // Wait until the alarm list is updated
        pthread_cond_wait(&alarm_cond, &alarm_mutex);

        // Array to track groups for which we have active threads but no alarms
        int groups_to_remove[MAX_GROUPS] = {0};

        // Check the alarm list for active groups
        alarm_t *current = alarm_list;
        while (current != NULL) {
            groups_to_remove[current->groupId] = 1; // Mark group as having an alarm
            current = current->link;               // Move to the next alarm
        }

        // Iterate over all possible groups and find threads to terminate
        for (int group_id = 0; group_id < MAX_GROUPS; group_id++) {
            if (active_group_threads[group_id] == 1 && groups_to_remove[group_id] == 0) {
                // Mark the thread as inactive
                active_group_threads[group_id] = 0;

                // Log the removal of the display thread
                char time_buffer[64];
                get_current_time(time_buffer, sizeof(time_buffer));
                printf("No More Alarms in Group(%d). Alarm Removal Thread Has Removed "
                       "Display Alarm Thread at %s: Group(%d)\n",
                       group_id, time_buffer, group_id);
            }
        }

        pthread_mutex_unlock(&alarm_mutex); // Unlock the mutex
    }
    return NULL;
}

int main (int argc, char *argv[])
{
    int status;
    char input[128];
    alarm_t *alarm;
    pthread_t thread;
    char command[50];
    int alarm_id, group_id, time;
    char message[128 - 50];

    pthread_t group_creation_thread, group_removal_thread;

    //Create the group display creation thread.
    if (pthread_create(&group_creation_thread, NULL, group_display_creation_thread, NULL) != 0) {
        fprintf(stderr, "Error: Unable to create group display creation thread\n");
        exit(1);
    }
    pthread_detach(group_creation_thread);

    // Create the group display removal thread
    if (pthread_create(&group_removal_thread, NULL, group_display_removal_thread, NULL) != 0) {
        fprintf(stderr, "Error: Unable to create group display removal thread\n");
        exit(1);
    }
    pthread_detach(group_removal_thread);
    

    while (1) {
        printf ("Alarm> ");
        if (fgets (input, sizeof (input), stdin) == NULL) exit (0);
        if (strlen (input) <= 1) continue;
        alarm = (alarm_t*)malloc (sizeof (alarm_t));
        if (alarm == NULL)
            errno_abort ("Allocate alarm");

        /*
         * Parsing input line to check what kind of request is being made.
         */
        if (sscanf(input, "Start_Alarm(%d): Group(%d) %d %[^\n]", &alarm_id, &group_id, &time, message) == 4) {
        if (alarm_id < 0 || group_id < 0 || time < 0) {
            handle_invalid_request();
        } else {
            printf("Start Alarm Request:\n");
            printf("  Alarm ID: %d\n", alarm_id);
            printf("  Group ID: %d\n", group_id);
            printf("  Time: %d seconds\n", time);
            printf("  Message: %s\n", message);
            insert_alarm(alarm_id, group_id, time, message);

            // Signal the condition variable to notify the group display creation thread
            pthread_cond_broadcast(&alarm_cond);
        }
    } else if (sscanf(input, "Change_Alarm(%d): Group(%d) %d %[^\n]", &alarm_id, &group_id, &time, message) == 4) {
        if (alarm_id < 0 || group_id < 0 || time < 0) {
            handle_invalid_request();
        } else {
            printf("Change Alarm Request:\n");
            printf("  Alarm ID: %d\n", alarm_id);
            printf("  Group ID: %d\n", group_id);
            printf("  Time: %d seconds\n", time);
            printf("  Message: %s\n", message);
        }
    } else if (sscanf(input, "Cancel_Alarm(%d)", &alarm_id) == 1) {
        if (alarm_id < 0) {
            handle_invalid_request();
        } else {
            printf("Cancel Alarm Request:\n");
            printf("  Alarm ID: %d\n", alarm_id);
        }
    } else if (sscanf(input, "Suspend_Alarm(%d)", &alarm_id) == 1) {
        if (alarm_id < 0) {
            handle_invalid_request();
        } else {
            printf("Suspend Alarm Request:\n");
            printf("  Alarm ID: %d\n", alarm_id);
        }
    } else if (sscanf(input, "Reactivate_Alarm(%d)", &alarm_id) == 1) {
        if (alarm_id < 0) {
            handle_invalid_request();
        } else {
            printf("Reactivate Alarm Request:\n");
            printf("  Alarm ID: %d\n", alarm_id);
        }
    } else if (strcmp(input, "View_Alarms\n") == 0) {
        printf("View Alarms at ");
    
        char time_buffer[64];
        get_current_time(time_buffer, sizeof(time_buffer));
        printf("%s:\n", time_buffer);

        pthread_mutex_lock(&alarm_mutex);  //Lock the mutex to safely access the alarm list
    
        //Iterate over all active groups with display threads and their alarms
        for (int group_id = 0; group_id < MAX_GROUPS; group_id++) {
          if (active_group_threads[group_id] == 1) {  //Check if there's an active display thread for this group
            //Print the display thread info
            printf("%d. Display Thread %ld Assigned:\n", group_id + 1, group_id);

            alarm_t *current = alarm_list;  //Pointer to traverse the alarm list
            int alarm_count = 0;

            // Traverse through the alarm list and print the alarms assigned to this group
            while (current != NULL) {
                if (current->groupId == group_id) {  // Check if the alarm belongs to this group
                    alarm_count++;
                    char assign_time_buffer[64];
                    get_current_time(assign_time_buffer, sizeof(assign_time_buffer));
                    printf(" %da. Alarm(%d): Created at %s Assigned at %s %s Status: Active\n",
                           alarm_count, current->id, time_buffer, assign_time_buffer, current->message);
                }
                current = current->link;  //Move to the next alarm in the list
            }

            //If no alarms assigned to this group, we can indicate that as well
            if (alarm_count == 0) {
                printf(" No alarms assigned to this thread.\n");
            }
        }
    }

    pthread_mutex_unlock(&alarm_mutex);  // Unlock the mutex after processing the list
    } else {
        handle_invalid_request();
    }
    }
}
