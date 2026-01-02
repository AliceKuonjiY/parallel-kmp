#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>

#define THREAD_COUNT 10
#define MAX_PATTERN_LENGTH (4096 * 2)
#define VIRUS_NUM 10
#define TEXT_BUF_SIZE (16 * 4096)
#define min(a, b) ((a) < (b) ? (a) : (b))

FILE *output;
int match_cnt = 0;
int match_flag = 0;
int match_pos[VIRUS_NUM];
char pattern[VIRUS_NUM][MAX_PATTERN_LENGTH];
int pi[VIRUS_NUM][MAX_PATTERN_LENGTH];
char *produced_text[THREAD_COUNT];
int produced_text_len[THREAD_COUNT];
int produced_done = 0;
int pattern_len[VIRUS_NUM];

pthread_mutex_t lock_match_pos = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_produce = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_can_produce = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond_can_consume = PTHREAD_COND_INITIALIZER;

void init(char *virus_path) {
    char virus[100];
    for (int t = 0; t < VIRUS_NUM; t++) {
        sprintf(virus, "%s/virus%02d.bin", virus_path, t + 1);
        FILE *pf = fopen(virus, "rb");
        if (!pf) {
            fprintf(stderr, "无法打开文本文件: %s\n", virus);
            return;
        }

        fseek(pf, 0, SEEK_END);
        pattern_len[t] = ftell(pf);
        fseek(pf, 0, SEEK_SET);

        fread(pattern[t], 1, pattern_len[t], pf);
        pi[t][0] = 0;
        int j = 0;
        for (int i = 1; i < pattern_len[t]; i++) {
            while (j > 0 && pattern[t][i] != pattern[t][j]) {
                j = pi[t][j - 1];
            }
            if (pattern[t][i] == pattern[t][j]) {
                j++;
            }
            pi[t][i] = j;
        }

        fclose(pf);
    }
}

void kmp_consumer() {
    while (1) {
        pthread_mutex_lock(&lock_produce);
        while (produced_done == 0) {
            pthread_cond_wait(&cond_can_consume, &lock_produce);
        }
        produced_done--;
        char *text_buf = produced_text[produced_done];
        size_t text_len = produced_text_len[produced_done];
        pthread_cond_broadcast(&cond_can_produce);
        pthread_mutex_unlock(&lock_produce);

        if (text_buf == NULL) {
            free(text_buf);
            break;
        }

        for (int t = 0; t < VIRUS_NUM; t++) {
            if (match_pos[t] == 1) {
                continue;
            }
            int i = 0;
            int j = 0;
            while (i < text_len) {
                if (pattern[t][j] == text_buf[i]) {
                    i++;
                    j++;
                }
                if (j == pattern_len[t]) {
                    match_pos[t] = 1;
                    match_flag = 1;
                    break;
                    j = pi[t][j - 1];
                } else if (i < text_len && pattern[t][j] != text_buf[i]) {
                    if (j != 0)
                        j = pi[t][j - 1];
                    else
                        i++;
                }
            }
        }

        free(text_buf);
    }
}

void kmp_producer(FILE *tf) {
    fseek(tf, 0, SEEK_SET);
    while (1) {
        char *text_buf = malloc(TEXT_BUF_SIZE + MAX_PATTERN_LENGTH + 1);
        size_t text_len = fread(text_buf, 1, TEXT_BUF_SIZE + MAX_PATTERN_LENGTH, tf);
        if (text_len >= TEXT_BUF_SIZE + MAX_PATTERN_LENGTH) {
            fseek(tf, -MAX_PATTERN_LENGTH, SEEK_CUR);
        }
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
        produced_done++;
        pthread_cond_broadcast(&cond_can_consume);
        pthread_mutex_unlock(&lock_produce);

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

void parllel(FILE *tf) {
    pthread_t producer_thread;
    pthread_t consumer_threads[THREAD_COUNT];

    char *text_buf = malloc(TEXT_BUF_SIZE + MAX_PATTERN_LENGTH);
    produced_done = 0;

    fseek(tf, 0, SEEK_END);
    long size = ftell(tf);
    fseek(tf, 0, SEEK_SET);

    int flag = 0;
    for (int i = 0; i < VIRUS_NUM; i++) {
        if (size > pattern_len[i]) {
            flag = 1;
            break;
        }
    }

    if (flag == 1) {
        pthread_create(&producer_thread, NULL, (void *)kmp_producer, (void *)tf);
        for (int i = 0; i < THREAD_COUNT; i++) {
            pthread_create(&consumer_threads[i], NULL, (void *)kmp_consumer, NULL);
        }

        pthread_join(producer_thread, NULL);
        for (int i = 0; i < THREAD_COUNT; i++) {
            pthread_join(consumer_threads[i], NULL);
        }
    }
}

void visit_dir(const char *basePath) {
    char path[1000];
    struct dirent *dp;
    DIR *dir = opendir(basePath);

    if (!dir) {
        fprintf(stderr, "无法打开目录 %s: %s\n", basePath, strerror(errno));
        return;
    }

    while ((dp = readdir(dir)) != NULL) {
        if (strcmp(dp->d_name, ".") == 0 ||
            strcmp(dp->d_name, "..") == 0) {
            continue;
        }

        sprintf(path, "%s/%s", basePath, dp->d_name);

        struct stat statbuf;
        if (stat(path, &statbuf) == -1) {
            fprintf(stderr, "无法获取文件信息: %s\n", path);
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            visit_dir(path);
        } else {
            FILE *tf = fopen(path, "rb");
            if (!tf) {
                fprintf(stderr, "无法打开文本文件: %s\n", path);
                return;
            }
            match_flag = 0;
            parllel(tf);
            fclose(tf);
            if (match_flag == 1) {
                fprintf(output, "%s", path);
            }
            for (int i = 0; i < VIRUS_NUM; i++) {
                if (match_pos[i] == 1) {
                    fprintf(output, " virus%02d.bin", i + 1);
                }
                match_pos[i] = 0;
            }
            if (match_flag == 1) {
                fprintf(output, "\n");
            }
        }
    }

    closedir(dir);
}

int main(int argc, char *argv[]) {
    output = fopen("./result_software.txt", "w+");
    if (output == NULL) {
        fprintf(stderr, "fail to open result_software.txt");
    }
    int len = strlen(argv[1]);
    if (argv[1][len - 1] == '/') {
        argv[1][len - 1] = '\0';
    }
    len = strlen(argv[2]);
    if (argv[2][len - 1] == '/') {
        argv[2][len - 1] = '\0';
    }
    FILE *tf, *pf;
    init(argv[2]);
    visit_dir(argv[1]);
    return 0;
}
