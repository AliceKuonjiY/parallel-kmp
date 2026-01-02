#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define THREAD_COUNT 4
#define MAX_PATTERN_LENGTH 1000
#define TEXT_BUF_SIZE (256 * 4096)
#define min(a, b) ((a) < (b) ? (a) : (b))

FILE *output;
int match_cnt = 0;
int match_pos[MAX_PATTERN_LENGTH];
char pattern[4096];
int pi[MAX_PATTERN_LENGTH];
char *produced_text[THREAD_COUNT];
int produced_text_len[THREAD_COUNT];
int produced_offset[THREAD_COUNT];
int produced_done = 0;
int pattern_len = 0;

pthread_mutex_t lock_match_pos = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_produce = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_can_produce = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond_can_consume = PTHREAD_COND_INITIALIZER;

void open_files(int argc, char *argv[], FILE **tf, FILE **pf) {
    if (argc != 3) {
        fprintf(stderr, "用法: %s <文本文件路径> <模式文件路径>\n", argv[0]);
        return;
    }
    const char *text_path = argv[1];
    const char *pattern_path = argv[2];
    *tf = fopen(text_path, "rt");
    if (!*tf) {
        fprintf(stderr, "无法打开文本文件: %s\n", text_path);
        return;
    }

    *pf = fopen(pattern_path, "rt");
    if (!*pf) {
        fprintf(stderr, "无法打开模式文件: %s\n", pattern_path);
        fclose(*tf);
        return;
    }
}

void init() {
    pi[0] = 0;
    int j = 0;
    for (int i = 1; i < pattern_len; i++) {
        while (j > 0 && pattern[i] != pattern[j]) {
            j = pi[j - 1];
        }
        if (pattern[i] == pattern[j]) {
            j++;
        }
        pi[i] = j;
    }
}

void kmp_consumer() {
    while (1) {
        pthread_mutex_lock(&lock_produce);
        while (produced_done == 0) {
            pthread_cond_wait(&cond_can_consume, &lock_produce);
        }
        char *text_buf = produced_text[produced_done - 1];
        size_t text_len = produced_text_len[produced_done - 1];
        int offset = produced_offset[produced_done - 1];
        if (text_buf == NULL) {
            produced_done--;
            pthread_cond_broadcast(&cond_can_produce);
            pthread_mutex_unlock(&lock_produce);
            break;
        }
        produced_done--;
        pthread_cond_broadcast(&cond_can_produce);
        pthread_mutex_unlock(&lock_produce);

        int i = 0;
        int j = 0;
        while (i < text_len) {
            if (pattern[j] == text_buf[i]) {
                i++;
                j++;
            }
            if (j == pattern_len) {
                pthread_mutex_lock(&lock_match_pos);
                match_pos[match_cnt++] = offset + i - j;
                pthread_mutex_unlock(&lock_match_pos);
                j = pi[j - 1];
            } else if (i < text_len && pattern[j] != text_buf[i]) {
                if (j != 0)
                    j = pi[j - 1];
                else
                    i++;
            }
        }

        free(text_buf);
    }
}

void kmp_producer(FILE *tf, FILE *pf) {
    int offset = 0;
    fseek(tf, 0, SEEK_SET);
    while (1) {
        char *text_buf = malloc(TEXT_BUF_SIZE + MAX_PATTERN_LENGTH);
        size_t text_len = fread(text_buf, 1, TEXT_BUF_SIZE + pattern_len, tf);
        if (!text_buf) {
            fprintf(stderr, "内存分配失败\n");
            break;
        }
        if (text_len == 0) {
            break;
        }

        pthread_mutex_lock(&lock_produce);
        while (produced_done == THREAD_COUNT) {
            pthread_cond_wait(&cond_can_produce, &lock_produce);
        }
        produced_text[produced_done] = text_buf;
        produced_text_len[produced_done] = text_len;
        produced_offset[produced_done] = offset;
        produced_done++;
        pthread_cond_broadcast(&cond_can_consume);
        pthread_mutex_unlock(&lock_produce);

        offset += text_len;
    }

    // 发送终止信号给消费者
    for (int i = 0; i < THREAD_COUNT + 1; i++) {
        pthread_mutex_lock(&lock_produce);
        while (produced_done != 0) {
            pthread_cond_wait(&cond_can_produce, &lock_produce);
        }
        produced_text[produced_done] = NULL;
        produced_text_len[produced_done] = i;
        produced_done++;
        pthread_cond_broadcast(&cond_can_consume);
        pthread_mutex_unlock(&lock_produce);
    }
}

void parllel(FILE *tf, FILE *pf) {
    pthread_t producer_thread;
    pthread_t consumer_threads[THREAD_COUNT];

    while (fgets(pattern, sizeof(pattern), pf)) {
        pattern_len = strlen(pattern);
        while (pattern_len > 0 && (pattern[pattern_len - 1] == '\n' || pattern[pattern_len - 1] == '\r')) {
            pattern[--pattern_len] = '\0';
        }
        if (pattern_len == 0) {
            continue;
        }
        init();

        match_cnt = 0;
        produced_done = 0;
        pthread_create(&producer_thread, NULL, (void *)kmp_producer, (void *)tf);
        for (int i = 0; i < THREAD_COUNT; i++) {
            pthread_create(&consumer_threads[i], NULL, (void *)kmp_consumer, NULL);
        }

        pthread_join(producer_thread, NULL);
        for (int i = 0; i < THREAD_COUNT; i++) {
            pthread_join(consumer_threads[i], NULL);
        }

        fprintf(output, "%d", match_cnt);
        for (int i = 0; i < match_cnt; i++) {
            fprintf(output, " %d", match_pos[i]);
        }
        fprintf(output, "\n");
    }
    fclose(pf);
    fclose(tf);
}

int main(int argc, char *argv[]) {
    output = fopen("./result_document.txt", "w+");
    if (output == NULL) {
        fprintf(stderr, "fail to open result_document.txt");
    }
    FILE *tf, *pf;
    open_files(argc, argv, &tf, &pf);
    parllel(tf, pf);
    return 0;
}
