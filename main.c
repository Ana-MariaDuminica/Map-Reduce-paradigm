#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>

#define MAX_RESULTS 100000
#define MAX_FILES 400
#define MAX_WORDS_PER_FILE 100000

typedef struct {
    char word[256];          // cuvantul
    int file_ids[MAX_FILES]; // lista id-urilor fisierelor in care apare cuvantul
    int file_count;          // numarul de fisiere in care apare cuvantul
} Entry;


typedef struct {
    int thread_id;      // id-ul thread-ului
    int num_threads_mapper;     // nr de thread-uri mapper
    int num_threads_reducer;        // nr de thread-uri reducer
    int num_files;      // nr de fisiere de procesat
    char** files;       // lista de fisiere
    Entry** mappers_lists;      // lista cu rezultate per fisier
    int* mappers_lists_counts;      // nr de cuvinte gasite in fiecare fisier
    pthread_mutex_t* mutex;     // mutex
    pthread_barrier_t* barrier;     // bariera pentru sincronizarea thread-urilor
    int* current_file_index;        // indexul fisierului curent
    Entry** alphabetical_list;      // matrice pentru stocarea cuvintelor procesate de mapperi
    int* alphabetical_list_counts;      // nr de intrari per rand din matrice
    int* processed_letters;     // marcatori pentru litere procesate
    pthread_mutex_t* row_mutexes;       // mutex-uri pentru fiecare rand din matrice
    int* reducer_processed_flags;       // Marcatori pentru rândurile procesate de reducer
} ThreadData;

// functie pentru eliminarea caracterelor non-alfabetice dintr-un cuvant
void sanitize_word(char* word) {
    size_t len = strlen(word);
    size_t write_index = 0;

    for (size_t read_index = 0; read_index < len; read_index++) {
        if (isalpha(word[read_index])) {
            word[write_index++] = word[read_index];
        }
    }
    word[write_index] = '\0';
}

// functie pentru transformarea unui sir de caractere in litere mici
void to_lowercase(char* str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower(str[i]);
    }
}

// functie de comparare pentru sortare
int compare_entries(const void* a, const void* b) {
    Entry* entry_a = (Entry*)a;
    Entry* entry_b = (Entry*)b;

    // sorteaza descrescator dupa nr de fisiere
    if (entry_a->file_count != entry_b->file_count) {
        return entry_b->file_count - entry_a->file_count;
    }

    // daca nr de fisiere este egal, sorteaza alfabetic
    return strcmp(entry_a->word, entry_b->word);
}

// Mapper function
void mapper_function(ThreadData* data) {
    while (1) {
        int file_index;

        // blocheaza mutex-ul pentru acces sigur la indexul curent
        pthread_mutex_lock(data->mutex);

        // verificam daca am procesat toate fisierele
        if (*data->current_file_index >= data->num_files) {
            pthread_mutex_unlock(data->mutex);
            break;
        }
        file_index = (*data->current_file_index)++; // obtine urmatorul fisier
        pthread_mutex_unlock(data->mutex);

        // obtine calea fisierului si id-ul acestuia
        char* file_path = data->files[file_index];
        int file_id = file_index;

        // deschide fisierul pentru citire
        FILE* file = fopen(file_path, "r");
        if (!file) {
            continue;
        }

        char word[256];
        while (fscanf(file, "%255s", word) == 1) {
            sanitize_word(word);  // elimina caracterele nevalide
            to_lowercase(word);   // transforma cuvantul in litere mici

            if (strlen(word) == 0) {
                continue;
            }

            Entry* result_row = data->mappers_lists[file_id];
            int* count = &data->mappers_lists_counts[file_id];

            // verifica daca cuvantul exista deja
            int found = 0;
            for (int j = 0; j < *count; j++) {
                if (strcmp(result_row[j].word, word) == 0) {
                    found = 1;
                    break;
                }
            }

            // daca nu exista, adauga cuvantul
            if (!found) {
                strcpy(result_row[*count].word, word);
                result_row[*count].file_ids[0] = file_id + 1;
                (*count)++;
            }
        }
        fclose(file);
    }
}

// Reducer function
void reducer_function(ThreadData* data) {
    while (1) {
        int row = -1;

        // gasim un rand neprocesat
        pthread_mutex_lock(data->mutex);
        for (int i = 0; i < data->num_files; i++) {
            if (!data->reducer_processed_flags[i]) { // daca randul nu este procesat
                row = i;
                data->reducer_processed_flags[i] = 1; // marcam randul ca procesat
                break;
            }
        }
        pthread_mutex_unlock(data->mutex);

        // daca nu mai sunt randuri de procesat, terminam
        if (row == -1) {
            break;
        }

        // procesam randul
        for (int j = 0; j < data->mappers_lists_counts[row]; j++) {
            char* word = data->mappers_lists[row][j].word;
            int file_id = data->mappers_lists[row][j].file_ids[0];

            // determina randul din matricea finala pe baza primei litere
            char first_letter = word[0];
            int alphabet_row = first_letter - 'a';

            if (alphabet_row < 0 || alphabet_row >= 26) {
                continue; // ignoram cuvintele cu caractere invalide
            }

            Entry* row_data = data->alphabetical_list[alphabet_row];
            int* count = &data->alphabetical_list_counts[alphabet_row];

            pthread_mutex_lock(&data->row_mutexes[alphabet_row]);

            // verifica daca cuvantul exista deja in rand
            int found = 0;
            for (int k = 0; k < *count; k++) {
                if (strcmp(row_data[k].word, word) == 0) {
                    // verifica daca id-ul fisierului exista deja
                    for (int l = 0; l < row_data[k].file_count; l++) {
                        if (row_data[k].file_ids[l] == file_id) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        row_data[k].file_ids[row_data[k].file_count++] = file_id;
                    }
                    found = 1;
                    break;
                }
            }

            // daca cuvantul nu exista, il adaugam
            if (!found) {
                strcpy(row_data[*count].word, word);
                row_data[*count].file_ids[0] = file_id;
                row_data[*count].file_count = 1;
                (*count)++;
            }

            pthread_mutex_unlock(&data->row_mutexes[alphabet_row]);
        }
    }
}

// scriere rezultate in fisiere
void write_results_to_files(ThreadData* data) {
    while (1) {
        int letter_to_process = -1;

        // gasim o litera care nu a fost procesata
        pthread_mutex_lock(data->mutex);
        for (int i = 0; i < 26; i++) {
            if (!data->processed_letters[i]) { // daca litera nu este procesata
                letter_to_process = i;
                data->processed_letters[i] = 1; // marcam ca procesata
                break;
            }
        }
        pthread_mutex_unlock(data->mutex);

        if (letter_to_process == -1) {
            break;
        }

        // procesam litera
        if (data->alphabetical_list_counts[letter_to_process] > 0) {
            char filename[16];
            sprintf(filename, "%c.txt", 'a' + letter_to_process); // generam numele fisierului

            FILE* output_file = fopen(filename, "w");
            if (!output_file) {
                continue;
            }

            Entry* row_data = data->alphabetical_list[letter_to_process];
            int row_count = data->alphabetical_list_counts[letter_to_process];

            qsort(row_data, row_count, sizeof(Entry), compare_entries);

            // scriem toate intrarile
            for (int j = 0; j < row_count; j++) {
                Entry* entry = &row_data[j];

                // sortam id-urile fisierelor
                for (int x = 0; x < entry->file_count - 1; x++) {
                    for (int y = x + 1; y < entry->file_count; y++) {
                        if (entry->file_ids[x] > entry->file_ids[y]) {
                            int temp = entry->file_ids[x];
                            entry->file_ids[x] = entry->file_ids[y];
                            entry->file_ids[y] = temp;
                        }
                    }
                }

                fprintf(output_file, "%s:[", entry->word);

                // scriem id-urile
                for (int k = 0; k < entry->file_count; k++) {
                    fprintf(output_file, "%d%s", entry->file_ids[k], 
                            (k == entry->file_count - 1) ? "" : " ");
                }

                fprintf(output_file, "]\n");
            }

            fclose(output_file);
        }
    }
}

// Thread Function
void* thread_function(void* arg) {
    ThreadData* data = (ThreadData*)arg;

    if (data->thread_id < data->num_threads_mapper) { // Mapper
        mapper_function(data);
    }

    // asteptam mapperii sa termine treaba
    // toate threadurile asteapta la bariera
    pthread_barrier_wait(data->barrier);

    if (data->thread_id >= data->num_threads_mapper) {
        reducer_function(data); // Reducer
    }

    // asteptam ca toti reducerii sa termine de procesat
    pthread_barrier_wait(data->barrier);

    if (data->thread_id >= data->num_threads_mapper) {
        // generam fisierele de iesire
        write_results_to_files(data);
    }

    return NULL;
}


int main(int argc, char* argv[]) {
    if (argc < 4) {
        return 1;
    }

    int num_threads_mapper = atoi(argv[1]); // numar threaduri mapper
    int num_threads_reducer = atoi(argv[2]); // numar threaduri reducer
    char* input_file = argv[3]; // input file
    int total_threads = num_threads_mapper + num_threads_reducer; // thread-urile totale

    // initializam threadurile
    pthread_t threads[total_threads];
    
    // initializam mutexul pentru agregarea listelor
    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, NULL);

    // initializam bariera
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, total_threads);

    // initializam mutex-urile pentru fiecare litera
    pthread_mutex_t row_mutexes[26];
    for (int i = 0; i < 26; i++) {
        pthread_mutex_init(&row_mutexes[i], NULL);
    }

    // citim fisierul de input
    FILE* file_list = fopen(input_file, "r");
    if (!file_list) {
        return 1;
    }

    int num_files; // nr de fisiere care trebuie parcurse
    if (fscanf(file_list, "%d", &num_files) != 1) {
        fclose(file_list);
        return 1;
    }
    fgetc(file_list);

    char** files = malloc(num_files * sizeof(char*)); // fisierele
    for (int i = 0; i < num_files; i++) {
        char line[256];
        if (!fgets(line, sizeof(line), file_list)) {
            fclose(file_list);
            return 1;
        }
        line[strcspn(line, "\n")] = '\0';
        files[i] = strdup(line);
    }
    fclose(file_list);

    // initializam listele pentru mapperi
    // matrice de tipul WordEntry (word + file_ids + file_count)
    // va avea atatea linii cate fisiere sunt
    // pe fiecare linie vor fi cuvintele dintr-un fisier
    Entry** mappers_lists = malloc(num_files * sizeof(Entry*));
    for (int i = 0; i < num_files; i++) {
        mappers_lists[i] = malloc(MAX_WORDS_PER_FILE * sizeof(Entry));
    }

    // nr de cuvinte total pentru fiecare fisier, initializat cu 0
    int* mappers_lists_counts = calloc(num_files, sizeof(int));

    // impartire dinamica a fisierelor pentru mapperi
    int current_file_index = 0;

    // lista alfabetica
    // matrice fiecare rand corespunde unei litere
    Entry** alphabetical_list = malloc(26 * sizeof(Entry*));
    for (int i = 0; i < 26; i++) {
        alphabetical_list[i] = malloc(MAX_RESULTS * sizeof(Entry));
    }

    // nr de cuvinte cu o litera
    int* alphabetical_list_counts = calloc(26, sizeof(int));

    // flag-uri pentru litere daca au fost procesate sau nu
    int* processed_letters = calloc(26, sizeof(int));

    // flag-uri pentru listele partiale daca au fost procesate sau nu
    int* reducer_processed_flags = calloc(num_files, sizeof(int));

    // crearea si initializarea ThreadData
    ThreadData* thread_data = malloc(total_threads * sizeof(ThreadData));
    for (int i = 0; i < total_threads; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].num_threads_mapper = num_threads_mapper;
        thread_data[i].num_threads_reducer = num_threads_reducer;
        thread_data[i].mutex = &mutex;
        thread_data[i].barrier = &barrier;
        thread_data[i].num_files = num_files;
        thread_data[i].files = files;

        // date ce vor fi folosite doar de mapperi
        thread_data[i].mappers_lists = mappers_lists;
        thread_data[i].mappers_lists_counts = mappers_lists_counts;
        thread_data[i].current_file_index = &current_file_index;

        // date ce vor fi folosite doar de reduceri
        thread_data[i].alphabetical_list = alphabetical_list;
        thread_data[i].alphabetical_list_counts = alphabetical_list_counts;
        thread_data[i].processed_letters = processed_letters;
        thread_data[i].row_mutexes = row_mutexes;
        thread_data[i].reducer_processed_flags = reducer_processed_flags;
    }

    // crearea thread-urilor
    for (int i = 0; i < total_threads; i++) {
        if (pthread_create(&threads[i], NULL, thread_function, &thread_data[i]) != 0) {
            return 1;
        }
    }

    // asteptarea threadurilor
    for (int i = 0; i < total_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // eliberarea resurselor
    for (int i = 0; i < num_files; i++) {
        free(files[i]);
    }
    free(files);

    for (int i = 0; i < num_files; i++) {
        free(mappers_lists[i]);
    }
    free(mappers_lists);

    for (int i = 0; i < 26; i++) {
        free(alphabetical_list[i]);
    }
    free(alphabetical_list);

    // eliberam flagurile auxiliare
    free(alphabetical_list_counts);
    free(processed_letters);
    free(reducer_processed_flags);

    // distrugem mutexurile pentru fiecare litera
    for (int i = 0; i < 26; i++) {
        pthread_mutex_destroy(&row_mutexes[i]);
    }

    // distrugem bariera si mutexul
    pthread_mutex_destroy(&mutex);
    pthread_barrier_destroy(&barrier);


    return 0;
}