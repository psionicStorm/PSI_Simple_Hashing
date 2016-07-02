#include "psi_simple_hashing.h"

void psi_simple_hashing(PSI_SIMPLE_HASHING_CTX * ctx) {
    ctx->queues = psi_sh_allocate_queues(ctx);
    parse_paths(ctx);
    show_settings(ctx);

    handle_input(ctx);
    psi_sh_create_table(ctx);

    psi_sh_free_queues(ctx);
}

static void show_settings(PSI_SIMPLE_HASHING_CTX *ctx) {
    ctx->size_source = fsize(ctx->path_source);
    printf("Source path : %s\n", ctx->path_source);
    printf("Buckets path : %s\n", ctx->path_buckets);
    printf("Queues buffer size : %zu\n", ctx->queue_buffer_size);
    printf("Source size : %zu\n", ctx->size_source);
    printf("Table size : %f\n", ctx->table_size);
    printf("Seeds :\n");
    for (uint8_t i = 0; i < ctx->hash_n; i++) {
        printf("%02x%02x%02x%02x%02x%02x%02x%02x",
                ctx->seed[i][0], ctx->seed[i][1], ctx->seed[i][2],
                ctx->seed[i][3], ctx->seed[i][4], ctx->seed[i][5],
                ctx->seed[i][6], ctx->seed[i][7]);

        printf("%02x%02x%02x%02x%02x%02x%02x%02x\n",
                ctx->seed[i][8], ctx->seed[i][9], ctx->seed[i][10],
                ctx->seed[i][11], ctx->seed[i][12], ctx->seed[i][13],
                ctx->seed[i][14], ctx->seed[i][15]);
    }
}

PSI_Queue ** psi_sh_allocate_queues(PSI_SIMPLE_HASHING_CTX * ctx) {
    PSI_Queue ** queues = (PSI_Queue**) malloc(ctx->bucket_n * sizeof (*queues));
    for (size_t i = 0; i < ctx->bucket_n; i++) {
        queues[i] = g_slice_alloc(sizeof (*queues[i]));
        slice_alloc_byte_buffer(&queues[i]->buffer, ctx->queue_buffer_size, 17);
    }
    return queues;
}

void psi_sh_free_queues(PSI_SIMPLE_HASHING_CTX * ctx) {
    for (size_t i = 0; i < ctx->bucket_n; i++) {
        slice_free_byte_buffer(&ctx->queues[i]->buffer, ctx->queue_buffer_size, 17);
        g_slice_free1(sizeof (*ctx->queues[i]), ctx->queues[i]);
    }
    free(ctx->queues);
}

void parse_paths(PSI_SIMPLE_HASHING_CTX * ctx) {
    char buf[128];
    strncpy(buf, ctx->path_root, 128);
    strncat(buf, ctx->path_source, 32);
    strncpy(ctx->path_source, buf, 128);
    snprintf(ctx->path_source + strlen(ctx->path_source), 16, "%d", (int) pow(10, ctx->element_pow));

    strncpy(buf, ctx->path_root, 128);
    strncat(buf, ctx->path_buckets, 32);
    strncpy(ctx->path_buckets, buf, 128);
}

static void handle_input(PSI_SIMPLE_HASHING_CTX * ctx) {
    psi_mkdir(ctx->path_buckets);
    ctx->divisor = 0xFFFFFFFFFFFFFFFF / ctx->bucket_n;

    FILE * f_source = fopen(ctx->path_source, "rb");
    if (f_source == NULL) {
        perror("Error opening source file");
        exit(EXIT_FAILURE);
    }

    uint8_t **buf;
    slice_alloc_byte_buffer(&buf, ctx->read_buffer_size, 17);


    for (size_t i = 0; i < ctx->read_buffer_size; i++) {
        if (fread(buf[i], 16, 1, f_source) < 1) {
            save_buffer(ctx, buf, i);
            save_all_buckets(ctx);
            break;
        } else if (i == ctx->read_buffer_size - 1) {
            save_buffer(ctx, buf, i);
            i = 0;
        }
    }


    slice_free_byte_buffer(&buf, ctx->read_buffer_size, 17);
    fclose(f_source);
}

static void save_bucket(PSI_SIMPLE_HASHING_CTX * ctx, uint64_t * n) {
    char tmp[128];
    strncpy(tmp, ctx->path_buckets, 110);
    snprintf(tmp + strlen(tmp), 16, "%"PRIu64, *n);

    FILE * f = fopen(tmp, "ab");
    for (size_t i = 0; i < ctx->queues[*n]->counter; i++)
        if (fwrite(ctx->queues[*n]->buffer[i], 17, 1, f) < 1)
            printf("Error writing to bucket\n");
    ctx->queues[*n]->counter = 0;
    fclose(f);
}

static void save_buffer(PSI_SIMPLE_HASHING_CTX * ctx, uint8_t ** buf, size_t n) {
#pragma omp parallel for shared(n, buf, ctx) num_threads(ctx->threads)
    for (size_t i = 0; i < n; i++) {
        uint64_t bucket;
        for (uint8_t j = 0; j < ctx->hash_n; j++) {
            psi_get_64bit_sha256_with_seed(ctx->seed[j], buf[i], &bucket);
            bucket /= ctx->divisor;
            add_to_bucket(ctx, ctx->queues[bucket], buf[i], j, &bucket);
        }
    }
}

static void add_to_bucket(PSI_SIMPLE_HASHING_CTX * ctx, PSI_Queue * q, uint8_t * buf, uint8_t hash_n, uint64_t * bucket) {
    while (q->counter == ctx->queue_buffer_size);
#pragma omp critical
    {
        if (q->counter > ctx->queue_buffer_size)
            printf("Queue counter is out of bounds\n");

        memcpy(q->buffer[q->counter], buf, 16);
        q->buffer[q->counter][17] = hash_n;
        q->counter++;
        if (q->counter == ctx->queue_buffer_size) {

            save_bucket(ctx, bucket);
        } else if (q->counter > ctx->queue_buffer_size)
            printf("Queue counter is out of bounds\n");
    }
}

static void save_all_buckets(PSI_SIMPLE_HASHING_CTX * ctx) {
    for (uint64_t i = 0; i < ctx->bucket_n; i++)
        save_bucket(ctx, &i);
}

static void psi_sh_create_table(PSI_SIMPLE_HASHING_CTX * ctx) {
    uint8_t buf[17 * 1000];
    int read, counter = 0, counter_result = -1;
    uint64_t pointer, table_divisor = (0xFFFFFFFFFFFFFFFF /
            (pow(10, ctx->element_pow) * ctx->table_size));
    char path_buf[128], remove_buf[150], path_sh_result[128];

    FILE * f_result = psi_try_fopen(path_sh_result, "ab");
    size_t l_size = (1 + pow(10, ctx->element_pow)) / ctx->bucket_n;
    GList * lists[l_size];
    psi_sh_null_lists(lists, l_size);

    while (counter < ctx->bucket_n) {
        if (counter == 0 || (counter + 1) % (ctx->bucket_n / 4) == 0) {
            strncpy(path_buf, ctx->path_buckets, 110);
            snprintf(path_buf + strlen(path_buf), 16, "result%d", counter_result);
            fclose(f_result);
            counter_result++;

        }
        strncpy(path_buf, ctx->path_buckets, 110);
        snprintf(path_buf + strlen(path_buf), 16, "%d", counter);
        if (fsize(path_buf) == 0) {
            counter++;
            continue;
        }
        FILE * f = psi_try_fopen(ctx->path_buckets, "rb");
        while (read = psi_sh_read_from_bucket(f, buf) > 0) {
            if (read == 0)
                break;
            for (size_t i = 0; i < read; i++) {
                uint8_t * elem = (uint8_t*) malloc(17 * sizeof*elem);
                psi_get_64bit_sha256_with_seed(ctx->seed[elem[16]], elem, &pointer);
                pointer /= table_divisor;
                if (pointer < counter * table_divisor ||
                        pointer > (counter + 1) * table_divisor)
                    printf("Error assigning element\n");

                pointer %= l_size;
                psi_sh_save_elem_to_list(lists[pointer], elem);
            }
        }
        counter++;
        snprintf(remove_buf, 149, "rm %s", path_buf);
        if (remove(remove_buf))
            printf("Error deleting bucket file\n");
        psi_sh_save_lists(lists, f_result, l_size);
        psi_sh_clear_lists(lists, l_size);
        psi_sh_null_lists(lists, l_size);
    }
    printf("PSI Simple Hashing : Done\n");
}

static int psi_sh_read_from_bucket(FILE * f, uint8_t buf[]) {
    return fread(buf, 16, 1000, f);
}

static void psi_sh_save_elem_to_list(GList * l, uint8_t * elem) {
    g_list_append(l, elem);
}

static void psi_sh_save_lists(GList ** lists, FILE * f, size_t l_size) {
    for (size_t i = 0; i < l_size; i++) {
        if (lists[i] == NULL) {
            lists[i] = psi_sh_add_empty(lists[i]);
        }
        GSList * last = g_list_last(lists[i]);
        ((uint8_t*) last->data)[16] | LAST_ELEM_FLAG;
        g_list_foreach(lists[i], psi_sh_save_elem_to_res, f);
    }
}

static void psi_sh_clear_lists(GList ** lists, size_t l_size) {
    for (size_t i = 0; i < l_size; i++)
        g_list_remove_all(lists[i], free);
}

static void psi_sh_null_lists(GList ** lists, size_t l_size) {
    for (size_t i = 0; i < l_size; i++)
        lists[i] = NULL;
}

static void psi_sh_save_elem_to_res(gpointer elem, gpointer f) {
    if (write((uint8_t*) elem, 17, 1, (FILE*) f) < 1)
        printf("Error writing result to file\n");
}

static GList * psi_sh_add_empty(GList * l) {
    uint8_t * elem = (uint8_t*) calloc(17 * sizeof*elem, 1);
    g_list_append(l, elem);
}