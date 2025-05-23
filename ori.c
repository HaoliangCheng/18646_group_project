#define _POSIX_C_SOURCE 199309L // clock_gettime

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ─────────────── 内存检查宏 ───────────────
#define CHECK_PTR(p, msg)                      \
    if (!(p))                                  \
    {                                          \
        printf("Failed to malloc: %s\n", msg); \
        exit(1);                               \
    }

// ─────────────── 超参数（已缩小） ───────────────
#define OLD_INPUT_SIZE 28
#define NEW_INPUT_SIZE 256
#define INPUT_NODES (NEW_INPUT_SIZE * NEW_INPUT_SIZE)
#define HIDDEN_NODES 256
#define OUTPUT_NODES 10
#define MAX_PATH 256
#define DEFAULT_TRAIN_IMAGES "train_images.bin"
#define DEFAULT_TRAIN_LABELS "train_labels.bin"
#define DEFAULT_MODEL "model.bin"
#define MAX_TRAIN 100
#define MAX_TEST 10

// ─────────────── dynamic data global variables ───────────────
double *training_images = NULL;         // [MAX_TRAIN][28*28]
double *test_images = NULL;             // [MAX_TEST][28*28]
double *training_images_resized = NULL; // [MAX_TRAIN][INPUT_NODES]
double *test_images_resized = NULL;     // [MAX_TEST][INPUT_NODES]
double *training_labels = NULL;         // [MAX_TRAIN][OUTPUT_NODES]
double *test_labels = NULL;             // [MAX_TEST][OUTPUT_NODES]
double *weight1 = NULL;                 // [INPUT_NODES][HIDDEN_NODES]
double *weight2 = NULL;                 // [HIDDEN_NODES][OUTPUT_NODES]
double *bias1 = NULL;                   // [HIDDEN_NODES]
double *bias2 = NULL;                   // [OUTPUT_NODES]

int correct_predictions;
int forward_prob_output;

// ─────────────── time helper ───────────────
static double g_ff_time = 0, g_bp_time = 0, g_wu_time = 0;
static inline double diff_sec(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

// ReLU及其导数
double relu(double x) { return x > 0 ? x : 0; }
double relu_derivative(double x) { return x > 0 ? 1 : 0; }
double sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }
int max_index(double *arr, int size)
{
    int max_i = 0;
    for (int i = 1; i < size; i++)
        if (arr[i] > arr[max_i])
            max_i = i;
    return max_i;
}

void input_string(const char *prompt, char *buf, int maxlen, const char *default_val)
{
    printf("%s (default: %s): ", prompt, default_val);
    if (fgets(buf, maxlen, stdin) == NULL)
        buf[0] = 0;
    int len = strlen(buf);
    if (len && buf[len - 1] == '\n')
        buf[len - 1] = 0;
    if (strlen(buf) == 0)
        strncpy(buf, default_val, maxlen);
}

// resize image
void resize_28_to_256(const double *src, double *dst)
{
    int new_size = NEW_INPUT_SIZE;
    int old_size = OLD_INPUT_SIZE;
    for (int y = 0; y < new_size; ++y)
    {
        int src_y = y * old_size / new_size;
        for (int x = 0; x < new_size; ++x)
        {
            int src_x = x * old_size / new_size;
            dst[y * new_size + x] = src[src_y * old_size + src_x];
        }
    }
}

// load images
int load_images(const char *filename, double *arr, int max_count)
{
    FILE *f = fopen(filename, "rb");
    if (!f)
    {
        printf("Error opening %s\n", filename);
        exit(1);
    }
    fseek(f, 16, SEEK_SET);
    int i, j;
    unsigned char pixel;
    for (i = 0; i < max_count; i++)
        for (j = 0; j < OLD_INPUT_SIZE * OLD_INPUT_SIZE; j++)
            if (fread(&pixel, 1, 1, f) != 1)
                goto END;
            else
                arr[i * OLD_INPUT_SIZE * OLD_INPUT_SIZE + j] = pixel / 255.0;
END:
    fclose(f);
    return i;
}
int load_labels(const char *filename, double *arr, int max_count)
{
    FILE *f = fopen(filename, "rb");
    if (!f)
    {
        printf("Error opening %s\n", filename);
        exit(1);
    }
    fseek(f, 8, SEEK_SET);
    int i, j;
    unsigned char label;
    for (i = 0; i < max_count; i++)
    {
        if (fread(&label, 1, 1, f) != 1)
            goto END;
        for (j = 0; j < OUTPUT_NODES; j++)
            arr[i * OUTPUT_NODES + j] = (j == label) ? 1.0 : 0.0;
    }
END:
    fclose(f);
    return i;
}

// save / load weights file
void save_weights_biases(const char *file_name)
{
    FILE *file = fopen(file_name, "wb");
    if (!file)
    {
        printf("Error opening file to save model\n");
        exit(1);
    }
    fwrite(weight1, sizeof(double), INPUT_NODES * HIDDEN_NODES, file);
    fwrite(weight2, sizeof(double), HIDDEN_NODES * OUTPUT_NODES, file);
    fwrite(bias1, sizeof(double), HIDDEN_NODES, file);
    fwrite(bias2, sizeof(double), OUTPUT_NODES, file);
    fclose(file);
}
void load_weights_biases(const char *file_name)
{
    FILE *file = fopen(file_name, "rb");
    if (!file)
    {
        printf("Error opening file to load model\n");
        exit(1);
    }
    fread(weight1, sizeof(double), INPUT_NODES * HIDDEN_NODES, file);
    fread(weight2, sizeof(double), HIDDEN_NODES * OUTPUT_NODES, file);
    fread(bias1, sizeof(double), HIDDEN_NODES, file);
    fread(bias2, sizeof(double), OUTPUT_NODES, file);
    fclose(file);
}

// forward / backward / update
void train(double *input, double *output, int correct_label)
{
    struct timespec t0, t1, t2, t3;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    double hidden[HIDDEN_NODES];
    double output_layer[OUTPUT_NODES];

    for (int i = 0; i < HIDDEN_NODES; i++)
    {
        double sum = bias1[i];
        for (int j = 0; j < INPUT_NODES; j++)
            sum += input[j] * weight1[j * HIDDEN_NODES + i];
        hidden[i] = relu(sum);
    }
    for (int i = 0; i < OUTPUT_NODES; i++)
    {
        double sum = bias2[i];
        for (int j = 0; j < HIDDEN_NODES; j++)
            sum += hidden[j] * weight2[j * OUTPUT_NODES + i];
        output_layer[i] = sigmoid(sum);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_ff_time += diff_sec(t0, t1);

    if (max_index(output_layer, OUTPUT_NODES) == correct_label)
        forward_prob_output++;

    double error[OUTPUT_NODES], delta2[OUTPUT_NODES];
    for (int i = 0; i < OUTPUT_NODES; i++)
    {
        error[i] = output[i] - output_layer[i];
        delta2[i] = error[i] * output_layer[i] * (1 - output_layer[i]);
    }
    double delta1[HIDDEN_NODES] = {0};
    for (int i = 0; i < HIDDEN_NODES; i++)
    {
        double sum = 0;
        for (int j = 0; j < OUTPUT_NODES; j++)
            sum += delta2[j] * weight2[i * OUTPUT_NODES + j];
        delta1[i] = sum * relu_derivative(hidden[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    g_bp_time += diff_sec(t1, t2);

    const double lr = 0.05;
    for (int i = 0; i < INPUT_NODES; i++)
        for (int j = 0; j < HIDDEN_NODES; j++)
            weight1[i * HIDDEN_NODES + j] += lr * delta1[j] * input[i];
    for (int i = 0; i < HIDDEN_NODES; i++)
    {
        bias1[i] += lr * delta1[i];
        for (int j = 0; j < OUTPUT_NODES; j++)
            weight2[i * OUTPUT_NODES + j] += lr * delta2[j] * hidden[i];
    }
    for (int i = 0; i < OUTPUT_NODES; i++)
        bias2[i] += lr * delta2[i];

    clock_gettime(CLOCK_MONOTONIC, &t3);
    g_wu_time += diff_sec(t2, t3);
}

// test
void test(double *input, int correct_label)
{
    double hidden[HIDDEN_NODES], output_layer[OUTPUT_NODES];
    for (int i = 0; i < HIDDEN_NODES; i++)
    {
        double sum = bias1[i];
        for (int j = 0; j < INPUT_NODES; j++)
            sum += input[j] * weight1[j * HIDDEN_NODES + i];
        hidden[i] = relu(sum);
    }
    for (int i = 0; i < OUTPUT_NODES; i++)
    {
        double sum = bias2[i];
        for (int j = 0; j < HIDDEN_NODES; j++)
            sum += hidden[j] * weight2[j * OUTPUT_NODES + i];
        output_layer[i] = sigmoid(sum);
    }
    if (max_index(output_layer, OUTPUT_NODES) == correct_label)
        correct_predictions++;
}

// init weights
void init_weights()
{
    srand(time(NULL));
    double w1_lim = sqrt(6.0 / (INPUT_NODES + HIDDEN_NODES));
    double w2_lim = sqrt(6.0 / (HIDDEN_NODES + OUTPUT_NODES));
    for (int i = 0; i < INPUT_NODES; i++)
        for (int j = 0; j < HIDDEN_NODES; j++)
            weight1[i * HIDDEN_NODES + j] = ((double)rand() / RAND_MAX * 2 - 1) * w1_lim;
    for (int i = 0; i < HIDDEN_NODES; i++)
        bias1[i] = 0;
    for (int i = 0; i < HIDDEN_NODES; i++)
        for (int j = 0; j < OUTPUT_NODES; j++)
            weight2[i * OUTPUT_NODES + j] = ((double)rand() / RAND_MAX * 2 - 1) * w2_lim;
    for (int i = 0; i < OUTPUT_NODES; i++)
        bias2[i] = 0;
}

// training mode
void train_mode()
{
    char train_img_path[MAX_PATH], train_label_path[MAX_PATH];
    char model_path[MAX_PATH], buf[32];
    int epochs = 10;

    input_string("Enter train image file path", train_img_path, MAX_PATH, DEFAULT_TRAIN_IMAGES);
    input_string("Enter train label file path", train_label_path, MAX_PATH, DEFAULT_TRAIN_LABELS);
    input_string("Enter model save path", model_path, MAX_PATH, DEFAULT_MODEL);
    printf("Enter number of epochs (default: 10): ");
    if (fgets(buf, 32, stdin) && buf[0] != '\n')
        epochs = atoi(buf);
    if (epochs < 1)
        epochs = 10;

    struct timespec ld_s, ld_e;
    clock_gettime(CLOCK_MONOTONIC, &ld_s);
    int train_count = load_images(train_img_path, training_images, MAX_TRAIN);
    int label_count = load_labels(train_label_path, training_labels, MAX_TRAIN);
    clock_gettime(CLOCK_MONOTONIC, &ld_e);
    double ld_time = diff_sec(ld_s, ld_e);
    if (label_count != train_count)
    {
        printf("Image/Label mismatch!\n");
        exit(1);
    }
    printf("Train set: imgs %d | labels %d | load %.3fs\n", train_count, label_count, ld_time);


    for (int i = 0; i < train_count; ++i)
        resize_28_to_256(training_images + i * OLD_INPUT_SIZE * OLD_INPUT_SIZE, training_images_resized + i * INPUT_NODES);


    printf("Initializing weights...\n");
    init_weights();

    printf("Start Training...\n");
    for (int epoch = 0; epoch < epochs; epoch++)
    {
        struct timespec e_s, e_e;
        g_ff_time = g_bp_time = g_wu_time = 0;
        forward_prob_output = 0;
        clock_gettime(CLOCK_MONOTONIC, &e_s);

        for (int i = 0; i < train_count; i++)
        {
            int correct_label = max_index(training_labels + i * OUTPUT_NODES, OUTPUT_NODES);
            train(training_images_resized + i * INPUT_NODES, training_labels + i * OUTPUT_NODES, correct_label);
        }

        clock_gettime(CLOCK_MONOTONIC, &e_e);
        double ep_time = diff_sec(e_s, e_e);

        printf("Epoch %d : imgs %d | Acc %.4f | "
               "Time %.3fs\n",
               epoch + 1, train_count,
               (double)forward_prob_output / train_count,
               ep_time);
    }
    save_weights_biases(model_path);
    printf("Model saved to %s\n", model_path);
}


// main program
int main()
{
    training_images = (double *)malloc(sizeof(double) * MAX_TRAIN * OLD_INPUT_SIZE * OLD_INPUT_SIZE);
    CHECK_PTR(training_images, "training_images");
    test_images = (double *)malloc(sizeof(double) * MAX_TEST * OLD_INPUT_SIZE * OLD_INPUT_SIZE);
    CHECK_PTR(test_images, "test_images");
    training_images_resized = (double *)malloc(sizeof(double) * MAX_TRAIN * INPUT_NODES);
    CHECK_PTR(training_images_resized, "training_images_resized");
    test_images_resized = (double *)malloc(sizeof(double) * MAX_TEST * INPUT_NODES);
    CHECK_PTR(test_images_resized, "test_images_resized");
    training_labels = (double *)malloc(sizeof(double) * MAX_TRAIN * OUTPUT_NODES);
    CHECK_PTR(training_labels, "training_labels");
    test_labels = (double *)malloc(sizeof(double) * MAX_TEST * OUTPUT_NODES);
    CHECK_PTR(test_labels, "test_labels");
    weight1 = (double *)malloc(sizeof(double) * INPUT_NODES * HIDDEN_NODES);
    CHECK_PTR(weight1, "weight1");
    weight2 = (double *)malloc(sizeof(double) * HIDDEN_NODES * OUTPUT_NODES);
    CHECK_PTR(weight2, "weight2");
    bias1 = (double *)malloc(sizeof(double) * HIDDEN_NODES);
    CHECK_PTR(bias1, "bias1");
    bias2 = (double *)malloc(sizeof(double) * OUTPUT_NODES);
    CHECK_PTR(bias2, "bias2");


    train_mode();


    free(training_images);
    free(test_images);
    free(training_images_resized);
    free(test_images_resized);
    free(training_labels);
    free(test_labels);
    free(weight1);
    free(weight2);
    free(bias1);
    free(bias2);

    return 0;
}