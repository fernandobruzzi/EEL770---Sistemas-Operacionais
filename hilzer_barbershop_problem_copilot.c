#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>
#include <getopt.h>

// Configurações (padrões que podem ser alterados via linha de comando)
typedef struct {
    int max_customers;
    int max_capacity;
    int num_barbers;
    int sofa_capacity;
    int min_haircut_time;
    int max_haircut_time;
    int min_payment_time;
    int max_payment_time;
    int min_arrival_interval;    // Intervalo mínimo entre chegadas (ms)
    int max_arrival_interval;    // Intervalo máximo entre chegadas (ms)
    int variability_factor;      // Fator de variabilidade (1-10)
    int simulation_time;         // Tempo total de simulação em segundos (0 = ilimitado)
} Config;

// Configuração global
Config config = {
    .max_customers = 50,
    .max_capacity = 10,        // Reduzido para criar mais pressão
    .num_barbers = 2,          // Reduzido para criar gargalo
    .sofa_capacity = 3,        // Reduzido para criar mais concorrência
    .min_haircut_time = 2000,  // Aumentado para criar mais demora
    .max_haircut_time = 8000,  // Aumentado significativamente
    .min_payment_time = 1000,  // Aumentado
    .max_payment_time = 4000,  // Aumentado
    .min_arrival_interval = 50,    // Chegadas mais frequentes
    .max_arrival_interval = 800,   // Mas com menos variação máxima
    .variability_factor = 7,       // Mais variabilidade
    .simulation_time = 60          // 60 segundos de simulação
};

// Estrutura para nó da fila FIFO
typedef struct QueueNode {
    int customer_id;
    struct QueueNode* next;
} QueueNode;

// Estrutura para fila FIFO
typedef struct Queue {
    QueueNode* head;
    QueueNode* tail;
    int size;
} Queue;

// Variáveis globais
int customers_in_shop = 0;
int customers_on_sofa = 0;
int customers_being_served = 0;
int customers_paying = 0;
int total_visits = 0;
int customers_attended = 0;
int program_should_stop = 0;
time_t simulation_start_time;  // Tempo de início da simulação

Queue* waiting_for_sofa;
Queue* waiting_for_chair;

// Mutexes e variáveis de condição
pthread_mutex_t shop_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t sofa_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t chair_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t payment_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t sofa_available = PTHREAD_COND_INITIALIZER;
pthread_cond_t chair_available = PTHREAD_COND_INITIALIZER;
pthread_cond_t barber_available = PTHREAD_COND_INITIALIZER;
pthread_cond_t customer_ready_to_pay = PTHREAD_COND_INITIALIZER;
pthread_cond_t payment_done = PTHREAD_COND_INITIALIZER;

// Funções para manejo de fila FIFO
Queue* createQueue() {
    Queue* q = (Queue*)malloc(sizeof(Queue));
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    return q;
}

void enqueue(Queue* q, int customer_id) {
    QueueNode* newNode = (QueueNode*)malloc(sizeof(QueueNode));
    newNode->customer_id = customer_id;
    newNode->next = NULL;
    
    if (q->tail == NULL) {
        q->head = q->tail = newNode;
    } else {
        q->tail->next = newNode;
        q->tail = newNode;
    }
    q->size++;
}

int dequeue(Queue* q) {
    if (q->head == NULL) return -1;
    
    QueueNode* temp = q->head;
    int customer_id = temp->customer_id;
    q->head = q->head->next;
    
    if (q->head == NULL) {
        q->tail = NULL;
    }
    
    free(temp);
    q->size--;
    return customer_id;
}

int isEmpty(Queue* q) {
    return q->size == 0;
}

// Função para obter timestamp
void getCurrentTime(char* buffer) {
    struct timeval tv;
    struct tm* timeinfo;
    gettimeofday(&tv, NULL);
    timeinfo = localtime(&tv.tv_sec);
    
    snprintf(buffer, 100, "[%02d:%02d:%02d.%03d]", 
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, (int)(tv.tv_usec/1000));
}

// Função para log thread-safe
void logMessage(const char* message) {
    pthread_mutex_lock(&log_mutex);
    char timestamp[100];
    getCurrentTime(timestamp);
    printf("%s %s\n", timestamp, message);
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
}

// Função para gerar tempo aleatório
int randomTime(int min_time, int max_time) {
    return min_time + (rand() % (max_time - min_time + 1));
}

// Função para gerar tempo aleatório com variabilidade aumentada
int variableRandomTime(int base_min, int base_max, int variability_factor) {
    // Aumenta o range baseado no fator de variabilidade (1-10)
    int range_expansion = variability_factor * 20; // 20ms por fator
    int new_min = base_min;
    int new_max = base_max + range_expansion;
    
    // Com 30% de chance, gera um tempo muito mais longo (picos de variabilidade)
    if (rand() % 100 < 30) {
        new_max = base_max + (range_expansion * 3);
    }
    
    return randomTime(new_min, new_max);
}

// Funções do cliente
void enterShop(int customer_id) {
    char log_msg[200];
    
    // Tempo para decidir entrar na loja (com variabilidade)
    usleep(variableRandomTime(50, 200, config.variability_factor) * 1000);
    
    pthread_mutex_lock(&shop_mutex);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Tentando entrar na loja", customer_id);
    logMessage(log_msg);
    
    if (customers_in_shop >= config.max_capacity) {
        snprintf(log_msg, sizeof(log_msg), "Cliente %d: Loja lotada - saindo (balk)", customer_id);
        logMessage(log_msg);
        total_visits++;
        pthread_mutex_unlock(&shop_mutex);
        return;
    }
    
    customers_in_shop++;
    total_visits++;
    enqueue(waiting_for_sofa, customer_id);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Entrou na loja (%d/%d) - aguardando lugar no sofá", 
             customer_id, customers_in_shop, config.max_capacity);
    logMessage(log_msg);
    
    pthread_mutex_unlock(&shop_mutex);
}

void sitOnSofa(int customer_id) {
    char log_msg[200];
    
    pthread_mutex_lock(&sofa_mutex);
    
    // Espera até haver lugar no sofá e ser sua vez
    while (customers_on_sofa >= config.sofa_capacity || 
           (waiting_for_sofa->head && waiting_for_sofa->head->customer_id != customer_id)) {
        snprintf(log_msg, sizeof(log_msg), "Cliente %d: Esperando lugar no sofá", customer_id);
        logMessage(log_msg);
        pthread_cond_wait(&sofa_available, &sofa_mutex);
    }
    
    // Remove da fila de espera do sofá
    if (!isEmpty(waiting_for_sofa) && waiting_for_sofa->head->customer_id == customer_id) {
        dequeue(waiting_for_sofa);
    }
    
    customers_on_sofa++;
    enqueue(waiting_for_chair, customer_id);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Conseguiu lugar no sofá (%d/%d) - esperando barbeiro", 
             customer_id, customers_on_sofa, config.sofa_capacity);
    logMessage(log_msg);
    
    // Tempo para se acomodar no sofá (com variabilidade)
    usleep(variableRandomTime(100, 300, config.variability_factor) * 1000);
    
    pthread_cond_signal(&chair_available);
    pthread_mutex_unlock(&sofa_mutex);
}

void getHairCut(int customer_id) {
    char log_msg[200];
    
    pthread_mutex_lock(&chair_mutex);
    
    // Espera até haver barbeiro disponível e ser sua vez
    while (customers_being_served >= config.num_barbers || 
           (waiting_for_chair->head && waiting_for_chair->head->customer_id != customer_id)) {
        snprintf(log_msg, sizeof(log_msg), "Cliente %d: Esperando barbeiro disponível", customer_id);
        logMessage(log_msg);
        pthread_cond_wait(&barber_available, &chair_mutex);
    }
    
    // Remove da fila de espera da cadeira
    if (!isEmpty(waiting_for_chair) && waiting_for_chair->head->customer_id == customer_id) {
        dequeue(waiting_for_chair);
    }
    
    customers_being_served++;
    
    // Libera lugar no sofá
    pthread_mutex_lock(&sofa_mutex);
    customers_on_sofa--;
    pthread_cond_signal(&sofa_available);
    pthread_mutex_unlock(&sofa_mutex);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Começou a cortar cabelo", customer_id);
    logMessage(log_msg);
    
    // Tempo para ir do sofá até a cadeira do barbeiro
    usleep(randomTime(50, 150) * 1000);
    
    pthread_mutex_unlock(&chair_mutex);
    
    // Simula tempo de corte (com variabilidade aumentada)
    int haircut_time = variableRandomTime(config.min_haircut_time, config.max_haircut_time, config.variability_factor);
    usleep(haircut_time * 1000);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Terminou de cortar cabelo", customer_id);
    logMessage(log_msg);
    
    // Tempo para sair da cadeira e ir para o caixa
    usleep(randomTime(100, 250) * 1000);
    
    pthread_mutex_lock(&chair_mutex);
    customers_being_served--;
    pthread_cond_signal(&barber_available);
    pthread_mutex_unlock(&chair_mutex);
}

void pay(int customer_id) {
    char log_msg[200];
    
    // Tempo para chegar ao caixa
    usleep(randomTime(80, 200) * 1000);
    
    pthread_mutex_lock(&payment_mutex);
    
    customers_paying++;
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Realizando pagamento", customer_id);
    logMessage(log_msg);
    
    // Avisa que há cliente para pagamento
    pthread_cond_signal(&customer_ready_to_pay);
    
    // IMPORTANTE: Acorda barbeiros que podem estar dormindo
    pthread_mutex_lock(&chair_mutex);
    pthread_cond_broadcast(&chair_available);
    pthread_mutex_unlock(&chair_mutex);
    
    // Espera o pagamento ser processado
    pthread_cond_wait(&payment_done, &payment_mutex);
    
    customers_paying--;
    customers_attended++;
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Pagamento concluído - saindo da loja", customer_id);
    logMessage(log_msg);
    
    // Tempo para sair da loja
    usleep(randomTime(50, 150) * 1000);
    
    pthread_mutex_lock(&shop_mutex);
    customers_in_shop--;
    pthread_mutex_unlock(&shop_mutex);
    
    pthread_mutex_unlock(&payment_mutex);
}

// Funções do barbeiro
void cutHair(int barber_id, int customer_id) {
    char log_msg[200];
    
    // Tempo para preparar instrumentos
    usleep(randomTime(100, 300) * 1000);
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Cortando cabelo do cliente %d", barber_id, customer_id);
    logMessage(log_msg);
}

void acceptPayment(int barber_id) {
    char log_msg[200];
    
    pthread_mutex_lock(&payment_mutex);
    
    // Espera cliente estar pronto para pagar
    while (customers_paying == 0 && !program_should_stop) {
        snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Esperando cliente para pagamento", barber_id);
        logMessage(log_msg);
        pthread_cond_wait(&customer_ready_to_pay, &payment_mutex);
    }
    
    // Se programa deve parar, sai sem processar
    if (program_should_stop) {
        pthread_mutex_unlock(&payment_mutex);
        return;
    }
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Processando pagamento de um cliente", barber_id);
    logMessage(log_msg);
    
    // Tempo para ir até o caixa
    usleep(randomTime(50, 150) * 1000);
    
    // Simula tempo de pagamento (com variabilidade aumentada)
    int payment_time = variableRandomTime(config.min_payment_time, config.max_payment_time, config.variability_factor);
    usleep(payment_time * 1000);
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Pagamento processado", barber_id);
    logMessage(log_msg);
    
    // Tempo para retornar do caixa
    usleep(randomTime(50, 120) * 1000);
    
    // Acorda apenas UM cliente (FIFO)
    pthread_cond_signal(&payment_done);
    pthread_mutex_unlock(&payment_mutex);
}

// Thread do cliente
void* customerThread(void* arg) {
    int customer_id = *(int*)arg;
    char log_msg[200];
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Chegou à barbearia", customer_id);
    logMessage(log_msg);
    
    // Tempo para observar a loja antes de entrar
    usleep(randomTime(50, 200) * 1000);
    
    enterShop(customer_id);
    
    // Se não conseguiu entrar, termina
    pthread_mutex_lock(&shop_mutex);
    int in_shop = 0;
    for (QueueNode* node = waiting_for_sofa->head; node != NULL; node = node->next) {
        if (node->customer_id == customer_id) {
            in_shop = 1;
            break;
        }
    }
    pthread_mutex_unlock(&shop_mutex);
    
    if (!in_shop) {
        return NULL;
    }
    
    // Tempo para processar ter entrado na loja
    usleep(randomTime(30, 100) * 1000);
    
    sitOnSofa(customer_id);
    
    // Tempo para relaxar no sofá antes de ser chamado
    usleep(randomTime(100, 300) * 1000);
    
    getHairCut(customer_id);
    
    // Tempo para avaliar o corte de cabelo
    usleep(randomTime(50, 200) * 1000);
    
    pay(customer_id);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Saiu da barbearia", customer_id);
    logMessage(log_msg);
    
    return NULL;
}

// Thread do barbeiro
void* barberThread(void* arg) {
    int barber_id = *(int*)arg;
    char log_msg[200];
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Iniciou trabalho", barber_id);
    logMessage(log_msg);
    
    while (!program_should_stop) {
        int did_work = 0; // Flag para rastrear se fez algum trabalho
        
        // PRIMEIRO: Verifica se há clientes para cortar cabelo
        pthread_mutex_lock(&chair_mutex);
        
        int customer_id = -1;
        if (!isEmpty(waiting_for_chair)) {
            customer_id = waiting_for_chair->head->customer_id;
            snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Atendendo cliente %d", barber_id, customer_id);
            logMessage(log_msg);
            did_work = 1;
        }
        
        pthread_mutex_unlock(&chair_mutex);
        
        if (customer_id != -1) {
            // Tempo para se preparar para o corte
            usleep(randomTime(50, 150) * 1000);
            
            // Chama a função cutHair que já tem o log específico
            cutHair(barber_id, customer_id);
            
            // Tempo para limpar após o corte
            usleep(randomTime(30, 100) * 1000);
        }
        
        // SEGUNDO: Sempre verifica se há clientes para pagamento
        pthread_mutex_lock(&payment_mutex);
        int has_paying_customers = customers_paying > 0;
        pthread_mutex_unlock(&payment_mutex);
        
        if (has_paying_customers && !program_should_stop) {
            // Tempo para ir até o caixa
            usleep(randomTime(50, 150) * 1000);
            
            acceptPayment(barber_id);
            
            // Tempo para retornar do caixa
            usleep(randomTime(50, 120) * 1000);
            
            did_work = 1;
        }
        
        // TERCEIRO: Se não fez nenhum trabalho, dorme esperando ser acordado
        if (!did_work && !program_should_stop) {
            pthread_mutex_lock(&chair_mutex);
            
            // Verifica novamente antes de dormir (double-check)
            if (isEmpty(waiting_for_chair) && !program_should_stop) {
                // Verifica se há clientes pagando antes de dormir
                pthread_mutex_lock(&payment_mutex);
                int still_has_paying = customers_paying > 0;
                pthread_mutex_unlock(&payment_mutex);
                
                if (!still_has_paying) {
                    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Dormindo - sem clientes", barber_id);
                    logMessage(log_msg);
                    pthread_cond_wait(&chair_available, &chair_mutex);
                }
            }
            
            pthread_mutex_unlock(&chair_mutex);
        }
        
        // Pequena pausa aleatória entre ciclos
        usleep(randomTime(80, 200) * 1000);
    }
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Terminou trabalho", barber_id);
    logMessage(log_msg);
    
    return NULL;
}

// Função para verificar condição de parada
void* monitorThread(void* arg) {
    (void)arg; // Suprime warning de parâmetro não usado
    while (1) {
        pthread_mutex_lock(&shop_mutex);
        
        int active_customers = customers_in_shop + customers_being_served + customers_paying;
        
        // Verifica tempo de simulação se configurado
        int time_exceeded = 0;
        if (config.simulation_time > 0) {
            time_t current_time = time(NULL);
            if (difftime(current_time, simulation_start_time) >= config.simulation_time) {
                time_exceeded = 1;
            }
        }
        
        // Para se atingiu o limite de clientes E não há clientes ativos
        // OU se excedeu o tempo de simulação
        if ((total_visits >= config.max_customers && active_customers == 0) || time_exceeded) {
            program_should_stop = 1;
            if (time_exceeded) {
                logMessage("Monitor: Tempo de simulação esgotado - finalizando programa");
            } else {
                logMessage("Monitor: Condição de parada atingida - finalizando programa");
            }
            
            pthread_mutex_unlock(&shop_mutex);
            
            // Acorda todos os barbeiros em todas as condições
            pthread_mutex_lock(&chair_mutex);
            pthread_cond_broadcast(&chair_available);
            pthread_mutex_unlock(&chair_mutex);
            
            pthread_mutex_lock(&payment_mutex);
            pthread_cond_broadcast(&customer_ready_to_pay);
            pthread_cond_broadcast(&payment_done);
            pthread_mutex_unlock(&payment_mutex);
            
            break;
        }
        
        pthread_mutex_unlock(&shop_mutex);
        usleep(500000); // Verifica a cada 500ms
    }
    
    return NULL;
}

// Função para exibir ajuda
void printUsage(const char* program_name) {
    printf("Uso: %s [OPÇÕES]\n", program_name);
    printf("\n");
    printf("Simulação do Problema da Barbearia do Hilzer\n");
    printf("\n");
    printf("OPÇÕES:\n");
    printf("  -c, --customers NUM      Número máximo de clientes (padrão: %d)\n", config.max_customers);
    printf("  -C, --capacity NUM       Capacidade máxima da loja (padrão: %d)\n", config.max_capacity);
    printf("  -b, --barbers NUM        Número de barbeiros (padrão: %d)\n", config.num_barbers);
    printf("  -s, --sofa NUM           Lugares no sofá (padrão: %d)\n", config.sofa_capacity);
    printf("  -t, --haircut-time MIN:MAX  Tempo de corte em ms (padrão: %d:%d)\n", 
           config.min_haircut_time, config.max_haircut_time);
    printf("  -p, --payment-time MIN:MAX  Tempo de pagamento em ms (padrão: %d:%d)\n", 
           config.min_payment_time, config.max_payment_time);
    printf("  -a, --arrival-time MIN:MAX  Intervalo entre chegadas em ms (padrão: %d:%d)\n", 
           config.min_arrival_interval, config.max_arrival_interval);
    printf("  -v, --variability NUM    Fator de variabilidade 1-10 (padrão: %d)\n", config.variability_factor);
    printf("  -T, --simulation-time NUM Tempo total de simulação em segundos (padrão: %d, 0=ilimitado)\n", config.simulation_time);
    printf("  -h, --help               Mostra esta ajuda\n");
    printf("\n");
    printf("EXEMPLOS:\n");
    printf("  %s                                    # Configuração padrão\n", program_name);
    printf("  %s -c 20 -b 2 -s 3                   # 20 clientes, 2 barbeiros, 3 lugares no sofá\n", program_name);
    printf("  %s -t 500:2000 -p 200:800            # Tempos mais rápidos\n", program_name);
    printf("  %s -v 8 -a 50:3000 -T 30             # Alta variabilidade, 30 segundos\n", program_name);
    printf("  %s --customers 100 --barbers 5       # Stress test\n", program_name);
    printf("\n");
    printf("CONFIGURAÇÕES PREDEFINIDAS:\n");
    printf("  Pequeno:  -c 10 -C 8 -b 2 -s 3\n");
    printf("  Padrão:   -c 50 -C 20 -b 3 -s 4\n");
    printf("  Grande:   -c 100 -C 30 -b 5 -s 6\n");
    printf("  Rápido:   -t 500:2000 -p 200:800\n");
    printf("  Lento:    -t 3000:8000 -p 1000:3000\n");
    printf("  Alta Variabilidade: -v 9 -a 50:4000\n");
}

// Função para parsear tempo no formato MIN:MAX
int parseTimeRange(const char* arg, int* min_time, int* max_time) {
    char* colon = strchr(arg, ':');
    if (!colon) {
        return 0; // Formato inválido
    }
    
    *colon = '\0'; // Separa a string
    *min_time = atoi(arg);
    *max_time = atoi(colon + 1);
    *colon = ':'; // Restaura a string
    
    if (*min_time <= 0 || *max_time <= 0 || *min_time >= *max_time) {
        return 0; // Valores inválidos
    }
    
    return 1; // Sucesso
}

// Função para parsear argumentos da linha de comando
int parseArguments(int argc, char* argv[]) {
    static struct option long_options[] = {
        {"customers",     required_argument, 0, 'c'},
        {"capacity",      required_argument, 0, 'C'},
        {"barbers",       required_argument, 0, 'b'},
        {"sofa",          required_argument, 0, 's'},
        {"haircut-time",  required_argument, 0, 't'},
        {"payment-time",  required_argument, 0, 'p'},
        {"arrival-time",  required_argument, 0, 'a'},
        {"variability",   required_argument, 0, 'v'},
        {"simulation-time", required_argument, 0, 'T'},
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    
    while ((c = getopt_long(argc, argv, "c:C:b:s:t:p:a:v:T:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                config.max_customers = atoi(optarg);
                if (config.max_customers <= 0) {
                    fprintf(stderr, "Erro: Número de clientes deve ser positivo\n");
                    return 0;
                }
                break;
                
            case 'C':
                config.max_capacity = atoi(optarg);
                if (config.max_capacity <= 0) {
                    fprintf(stderr, "Erro: Capacidade da loja deve ser positiva\n");
                    return 0;
                }
                break;
                
            case 'b':
                config.num_barbers = atoi(optarg);
                if (config.num_barbers <= 0) {
                    fprintf(stderr, "Erro: Número de barbeiros deve ser positivo\n");
                    return 0;
                }
                break;
                
            case 's':
                config.sofa_capacity = atoi(optarg);
                if (config.sofa_capacity <= 0) {
                    fprintf(stderr, "Erro: Número de lugares no sofá deve ser positivo\n");
                    return 0;
                }
                break;
                
            case 't':
                if (!parseTimeRange(optarg, &config.min_haircut_time, &config.max_haircut_time)) {
                    fprintf(stderr, "Erro: Formato de tempo de corte inválido. Use MIN:MAX (ex: 1000:5000)\n");
                    return 0;
                }
                break;
                
            case 'p':
                if (!parseTimeRange(optarg, &config.min_payment_time, &config.max_payment_time)) {
                    fprintf(stderr, "Erro: Formato de tempo de pagamento inválido. Use MIN:MAX (ex: 500:2000)\n");
                    return 0;
                }
                break;
                
            case 'a':
                if (!parseTimeRange(optarg, &config.min_arrival_interval, &config.max_arrival_interval)) {
                    fprintf(stderr, "Erro: Formato de intervalo de chegada inválido. Use MIN:MAX (ex: 100:2000)\n");
                    return 0;
                }
                break;
                
            case 'v':
                config.variability_factor = atoi(optarg);
                if (config.variability_factor < 1 || config.variability_factor > 10) {
                    fprintf(stderr, "Erro: Fator de variabilidade deve estar entre 1 e 10\n");
                    return 0;
                }
                break;
                
            case 'T':
                config.simulation_time = atoi(optarg);
                if (config.simulation_time < 0) {
                    fprintf(stderr, "Erro: Tempo de simulação não pode ser negativo (0 = ilimitado)\n");
                    return 0;
                }
                break;
                
            case 'h':
                printUsage(argv[0]);
                return 0;
                
            case '?':
                fprintf(stderr, "Use '%s --help' para ajuda\n", argv[0]);
                return 0;
                
            default:
                return 0;
        }
    }
    
    // Validações de consistência
    if (config.max_capacity < config.sofa_capacity) {
        fprintf(stderr, "Erro: Capacidade da loja (%d) deve ser >= lugares no sofá (%d)\n", 
                config.max_capacity, config.sofa_capacity);
        return 0;
    }
    
    if (config.max_capacity < config.num_barbers) {
        fprintf(stderr, "Erro: Capacidade da loja (%d) deve ser >= número de barbeiros (%d)\n", 
                config.max_capacity, config.num_barbers);
        return 0;
    }
    
    return 1; // Sucesso
}

int main(int argc, char* argv[]) {
    // Parseia argumentos da linha de comando
    if (!parseArguments(argc, argv)) {
        return 1;
    }
    srand(time(NULL));
    
    // Inicializa filas
    waiting_for_sofa = createQueue();
    waiting_for_chair = createQueue();
    
    logMessage("=== INICIANDO SIMULAÇÃO DA BARBEARIA DO HILZER ===");
    printf("Configurações: %d clientes máx, %d capacidade, %d barbeiros, %d lugares no sofá\n",
           config.max_customers, config.max_capacity, config.num_barbers, config.sofa_capacity);
    printf("Tempos: corte %d-%dms, pagamento %d-%dms, chegada %d-%dms\n",
           config.min_haircut_time, config.max_haircut_time, config.min_payment_time, config.max_payment_time,
           config.min_arrival_interval, config.max_arrival_interval);
    printf("Fator de variabilidade: %d/10", config.variability_factor);
    if (config.simulation_time > 0) {
        printf(", Tempo limite: %ds", config.simulation_time);
    }
    printf("\n");
    
    // Cria threads dos barbeiros
    pthread_t* barber_threads = malloc(config.num_barbers * sizeof(pthread_t));
    int* barber_ids = malloc(config.num_barbers * sizeof(int));
    
    for (int i = 0; i < config.num_barbers; i++) {
        barber_ids[i] = i + 1;
        pthread_create(&barber_threads[i], NULL, barberThread, &barber_ids[i]);
    }
    
    // Cria thread de monitoramento
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, monitorThread, NULL);
    
    // Cria threads dos clientes
    pthread_t* customer_threads = malloc(config.max_customers * sizeof(pthread_t));
    int* customer_ids = malloc(config.max_customers * sizeof(int));
    
    for (int i = 0; i < config.max_customers; i++) {
        customer_ids[i] = i + 1;
        pthread_create(&customer_threads[i], NULL, customerThread, &customer_ids[i]);
        
        // Intervalo muito variável entre chegadas de clientes
        usleep(variableRandomTime(config.min_arrival_interval, config.max_arrival_interval, config.variability_factor) * 1000);
    }
    
    // Espera todos os clientes terminarem
    for (int i = 0; i < config.max_customers; i++) {
        pthread_join(customer_threads[i], NULL);
    }
    
    // Espera monitor terminar
    pthread_join(monitor_thread, NULL);
    
    // Espera barbeiros terminarem
    for (int i = 0; i < config.num_barbers; i++) {
        pthread_join(barber_threads[i], NULL);
    }
    
    logMessage("=== SIMULAÇÃO FINALIZADA ===");
    printf("Total de visitas: %d\n", total_visits);
    printf("Total de clientes atendidos: %d\n", customers_attended);
    
    // Libera memória das filas e arrays
    free(waiting_for_sofa);
    free(waiting_for_chair);
    free(barber_threads);
    free(barber_ids);
    free(customer_threads);
    free(customer_ids);
    
    return 0;
}