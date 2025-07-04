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
    .variability_factor = 7        // Mais variabilidade
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

// Estado do cliente
typedef struct {
    int id;
    int is_getting_haircut;
    int haircut_done;
    int is_paying;
    int payment_done;
    int seated_in_chair;  // Nova flag para confirmar que cliente sentou
} CustomerState;

// Variáveis globais
int customers_in_shop = 0;
int customers_on_sofa = 0;
int customers_being_served = 0;
int customers_paying = 0;
int total_visits = 0;
int customers_attended = 0;
int program_should_stop = 0;

Queue* sofa_queue;    // Fila para o sofá
Queue* payment_queue; // Fila para pagamento

CustomerState* customer_states; // Array de estados dos clientes

// Mutexes simplificados
pthread_mutex_t shop_mutex = PTHREAD_MUTEX_INITIALIZER;     // Controla entrada/saída da loja
pthread_mutex_t sofa_mutex = PTHREAD_MUTEX_INITIALIZER;     // Controla sofá
pthread_mutex_t chair_mutex = PTHREAD_MUTEX_INITIALIZER;    // Controla cadeiras de corte
pthread_mutex_t payment_mutex = PTHREAD_MUTEX_INITIALIZER;  // Controla pagamentos
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;      // Para logs thread-safe

// Variáveis de condição
pthread_cond_t sofa_available = PTHREAD_COND_INITIALIZER;   // Lugar no sofá disponível
pthread_cond_t barber_available = PTHREAD_COND_INITIALIZER; // Barbeiro disponível
pthread_cond_t haircut_done = PTHREAD_COND_INITIALIZER;     // Corte terminado
pthread_cond_t payment_ready = PTHREAD_COND_INITIALIZER;    // Cliente pronto para pagar
pthread_cond_t payment_done_cond = PTHREAD_COND_INITIALIZER; // Pagamento processado
pthread_cond_t customer_seated = PTHREAD_COND_INITIALIZER;  // Cliente sentou na cadeira

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

// Seed thread-local para aleatoriedade
__thread unsigned int thread_seed = 0;

// Função para inicializar seed da thread
void initThreadSeed() {
    if (thread_seed == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        thread_seed = (unsigned int)(tv.tv_sec ^ tv.tv_usec ^ (unsigned long)pthread_self());
    }
}

// Função para gerar tempo aleatório thread-safe
int randomTime(int min_time, int max_time) {
    initThreadSeed();
    return min_time + (rand_r(&thread_seed) % (max_time - min_time + 1));
}

// Função para gerar tempo aleatório com variabilidade aumentada
int variableRandomTime(int base_min, int base_max, int variability_factor) {
    initThreadSeed();
    // Aumenta o range baseado no fator de variabilidade (1-10)
    int range_expansion = variability_factor * 20; // 20ms por fator
    int new_min = base_min;
    int new_max = base_max + range_expansion;
    
    // Com 30% de chance, gera um tempo muito mais longo (picos de variabilidade)
    if (rand_r(&thread_seed) % 100 < 30) {
        new_max = base_max + (range_expansion * 3);
    }
    
    return randomTime(new_min, new_max);
}

// Funções do cliente
int enterShop(int customer_id) {
    char log_msg[200];
    
    // Tempo para decidir entrar na loja
    usleep(variableRandomTime(50, 200, config.variability_factor) * 1000);
    
    pthread_mutex_lock(&shop_mutex);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Tentando entrar na loja", customer_id);
    logMessage(log_msg);
    
    // Verificação rigorosa da capacidade
    if (customers_in_shop >= config.max_capacity) {
        snprintf(log_msg, sizeof(log_msg), "Cliente %d: Loja lotada - saindo (balk)", customer_id);
        logMessage(log_msg);
        total_visits++;
        pthread_mutex_unlock(&shop_mutex);
        return 0; // Não conseguiu entrar
    }
    
    // Incrementa atomicamente
    customers_in_shop++;
    total_visits++;
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Entrou na loja (%d/%d)", 
             customer_id, customers_in_shop, config.max_capacity);
    logMessage(log_msg);
    
    pthread_mutex_unlock(&shop_mutex);
    return 1; // Conseguiu entrar
}

void sitOnSofa(int customer_id) {
    char log_msg[200];
    
    pthread_mutex_lock(&sofa_mutex);
    
    // Espera até haver lugar no sofá
    while (customers_on_sofa >= config.sofa_capacity) {
        snprintf(log_msg, sizeof(log_msg), "Cliente %d: Esperando lugar no sofá", customer_id);
        logMessage(log_msg);
        pthread_cond_wait(&sofa_available, &sofa_mutex);
    }
    
    customers_on_sofa++;
    enqueue(sofa_queue, customer_id);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Sentou no sofá (%d/%d) - esperando barbeiro", 
             customer_id, customers_on_sofa, config.sofa_capacity);
    logMessage(log_msg);
    
    // Acorda barbeiros
    pthread_cond_broadcast(&barber_available);
    
    pthread_mutex_unlock(&sofa_mutex);
    
    // Tempo no sofá
    usleep(variableRandomTime(100, 300, config.variability_factor) * 1000);
}

void getHairCut(int customer_id) {
    char log_msg[200];
    
    // Espera ser chamado pelo barbeiro - usa mutex separado para evitar deadlock
    pthread_mutex_lock(&shop_mutex);
    if (!customer_states[customer_id - 1].is_getting_haircut) {
        snprintf(log_msg, sizeof(log_msg), "Cliente %d: Esperando ser chamado para corte", customer_id);
        logMessage(log_msg);
        
        while (!customer_states[customer_id - 1].is_getting_haircut) {
            pthread_cond_wait(&barber_available, &shop_mutex);
        }
    }
    pthread_mutex_unlock(&shop_mutex);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Sentou na cadeira para corte", customer_id);
    logMessage(log_msg);
    
    // Marca que sentou na cadeira e avisa o barbeiro
    pthread_mutex_lock(&chair_mutex);
    customer_states[customer_id - 1].seated_in_chair = 1;
    pthread_cond_broadcast(&customer_seated);
    
    // Espera o corte terminar
    while (!customer_states[customer_id - 1].haircut_done) {
        pthread_cond_wait(&haircut_done, &chair_mutex);
    }
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Corte terminado - indo para pagamento", customer_id);
    logMessage(log_msg);
    
    // Reset o estado
    customer_states[customer_id - 1].is_getting_haircut = 0;
    customer_states[customer_id - 1].haircut_done = 0;
    customer_states[customer_id - 1].seated_in_chair = 0;
    
    pthread_mutex_unlock(&chair_mutex);
}

void pay(int customer_id) {
    char log_msg[200];
    
    // Tempo para ir ao caixa
    usleep(randomTime(80, 200) * 1000);
    
    pthread_mutex_lock(&payment_mutex);
    
    customers_paying++;
    customer_states[customer_id - 1].is_paying = 1;
    enqueue(payment_queue, customer_id);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Aguardando processar pagamento", customer_id);
    logMessage(log_msg);
    
    // Acorda barbeiro para processar pagamento
    pthread_cond_broadcast(&payment_ready);
    pthread_mutex_unlock(&payment_mutex);
    
    // Acorda barbeiro usando o mutex correto (shop_mutex)
    pthread_mutex_lock(&shop_mutex);
    pthread_cond_broadcast(&barber_available);
    pthread_mutex_unlock(&shop_mutex);
    
    // Volta a adquirir payment_mutex para esperar
    pthread_mutex_lock(&payment_mutex);
    
    // Espera pagamento ser processado
    while (!customer_states[customer_id - 1].payment_done) {
        pthread_cond_wait(&payment_done_cond, &payment_mutex);
    }
    
    customers_paying--;
    customer_states[customer_id - 1].is_paying = 0;
    customer_states[customer_id - 1].payment_done = 0;
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Pagamento concluído - saindo da loja", customer_id);
    logMessage(log_msg);
    
    pthread_mutex_unlock(&payment_mutex);
    
    usleep(randomTime(50, 150) * 1000);
}

// Funções do barbeiro
void cutHair(int barber_id, int customer_id) {
    char log_msg[200];
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Cortando cabelo do cliente %d", barber_id, customer_id);
    logMessage(log_msg);
    
    // Simula tempo de corte
    int haircut_time = variableRandomTime(config.min_haircut_time, config.max_haircut_time, config.variability_factor);
    usleep(haircut_time * 1000);
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Terminou corte do cliente %d", barber_id, customer_id);
    logMessage(log_msg);
}

void acceptPayment(int barber_id, int customer_id) {
    char log_msg[200];
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Processando pagamento do cliente %d", barber_id, customer_id);
    logMessage(log_msg);
    
    // Simula tempo de pagamento
    int payment_time = variableRandomTime(config.min_payment_time, config.max_payment_time, config.variability_factor);
    usleep(payment_time * 1000);
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Pagamento do cliente %d processado", barber_id, customer_id);
    logMessage(log_msg);
}

// Thread do barbeiro
void* barberThread(void* arg) {
    int barber_id = *(int*)arg;
    char log_msg[200];
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Iniciou trabalho", barber_id);
    logMessage(log_msg);
    
    while (!program_should_stop) {
        int did_work = 0;
        int customer_id = -1;
        
        // PRIMEIRO: Verifica se há cliente no sofá para cortar cabelo
        pthread_mutex_lock(&sofa_mutex);
        if (!isEmpty(sofa_queue)) {
            customer_id = dequeue(sofa_queue);
            did_work = 1;
            
            snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Chamando cliente %d para corte", barber_id, customer_id);
            logMessage(log_msg);
        }
        pthread_mutex_unlock(&sofa_mutex);
        
        if (customer_id != -1) {
            // Marca que cliente está sendo chamado para corte
            pthread_mutex_lock(&shop_mutex);
            customers_being_served++;
            customer_states[customer_id - 1].is_getting_haircut = 1;
            pthread_cond_broadcast(&barber_available); // Acorda cliente
            pthread_mutex_unlock(&shop_mutex);
            
            // CRUCIAL: Espera o cliente confirmar que sentou na cadeira
            pthread_mutex_lock(&chair_mutex);
            while (!customer_states[customer_id - 1].seated_in_chair) {
                pthread_cond_wait(&customer_seated, &chair_mutex);
            }
            pthread_mutex_unlock(&chair_mutex);
            
            // AGORA o cliente saiu do sofá e sentou na cadeira - libera lugar no sofá
            pthread_mutex_lock(&sofa_mutex);
            customers_on_sofa--;
            pthread_cond_broadcast(&sofa_available); // Libera lugar no sofá
            pthread_mutex_unlock(&sofa_mutex);
            
            // Agora sim pode cortar o cabelo (cliente já está sentado)
            cutHair(barber_id, customer_id);
            
            // Marca que corte terminou
            pthread_mutex_lock(&shop_mutex);
            customers_being_served--;
            pthread_mutex_unlock(&shop_mutex);
            
            pthread_mutex_lock(&chair_mutex);
            customer_states[customer_id - 1].haircut_done = 1;
            pthread_cond_broadcast(&haircut_done); // Acorda cliente
            pthread_mutex_unlock(&chair_mutex);
            
            customer_id = -1;
        }
        
        // SEGUNDO: Verifica se há cliente para pagamento
        pthread_mutex_lock(&payment_mutex);
        if (!isEmpty(payment_queue)) {
            customer_id = dequeue(payment_queue);
            did_work = 1;
            
            pthread_mutex_unlock(&payment_mutex);
            
            // Processa pagamento
            acceptPayment(barber_id, customer_id);
            
            // Marca pagamento como feito e incrementa contador de clientes atendidos
            pthread_mutex_lock(&payment_mutex);
            customers_attended++;
            customer_states[customer_id - 1].payment_done = 1;
            pthread_cond_broadcast(&payment_done_cond); // Acorda cliente
            pthread_mutex_unlock(&payment_mutex);
        } else {
            pthread_mutex_unlock(&payment_mutex);
        }
        
        // TERCEIRO: Se não fez trabalho, dorme esperando por corte ou pagamento
        if (!did_work && !program_should_stop) {
            snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Dormindo - sem trabalho", barber_id);
            logMessage(log_msg);
            
            // Escuta tanto por clientes no sofá quanto por pagamentos
            pthread_mutex_lock(&shop_mutex);
            pthread_cond_wait(&barber_available, &shop_mutex);
            pthread_mutex_unlock(&shop_mutex);
        }
        
        // Pequena pausa entre ciclos
        usleep(randomTime(50, 150) * 1000);
    }
    
    snprintf(log_msg, sizeof(log_msg), "Barbeiro %d: Terminou trabalho", barber_id);
    logMessage(log_msg);
    
    return NULL;
}

// Thread do cliente
void* customerThread(void* arg) {
    int customer_id = *(int*)arg;
    char log_msg[200];
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Chegou à barbearia", customer_id);
    logMessage(log_msg);
    
    // Tempo para observar a loja antes de entrar
    usleep(randomTime(50, 200) * 1000);
    
    if (!enterShop(customer_id)) {
        return NULL; // Não conseguiu entrar (balk)
    }
    
    // Processo completo: sofá -> corte -> pagamento
    sitOnSofa(customer_id);
    getHairCut(customer_id);
    pay(customer_id);
    
    // Sai da loja AQUI
    pthread_mutex_lock(&shop_mutex);
    customers_in_shop--;
    pthread_mutex_unlock(&shop_mutex);
    
    snprintf(log_msg, sizeof(log_msg), "Cliente %d: Saiu da barbearia", customer_id);
    logMessage(log_msg);
    
    return NULL;
}

// Função para verificar condição de parada
void* monitorThread(void* arg) {
    (void)arg; // Suprime warning de parâmetro não usado
    while (1) {
        // Lê variáveis de diferentes mutexes de forma segura
        pthread_mutex_lock(&shop_mutex);
        int shop_customers = customers_in_shop;
        int being_served = customers_being_served;
        pthread_mutex_unlock(&shop_mutex);
        
        pthread_mutex_lock(&payment_mutex);
        int paying_customers = customers_paying;
        pthread_mutex_unlock(&payment_mutex);
        
        int active_customers = shop_customers + being_served + paying_customers;
        
        // Debug: verifica inconsistências
        if (shop_customers > config.max_capacity) {
            char debug_msg[200];
            snprintf(debug_msg, sizeof(debug_msg), "ERRO: Loja com %d clientes (máx %d)!", 
                     shop_customers, config.max_capacity);
            logMessage(debug_msg);
        }
        
        if (total_visits >= config.max_customers && active_customers == 0) {
            program_should_stop = 1;
            
            char debug_msg[200];
            snprintf(debug_msg, sizeof(debug_msg), "Monitor: Condição de parada - visitas=%d, ativos=%d, atendidos=%d", 
                     total_visits, active_customers, customers_attended);
            logMessage(debug_msg);
            logMessage("Monitor: Condição de parada atingida - finalizando programa");
            
            // Acorda todos os barbeiros
            pthread_cond_broadcast(&barber_available);
            pthread_cond_broadcast(&payment_ready);
            
            break;
        }
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
    printf("  -h, --help               Mostra esta ajuda\n");
    printf("\n");
    printf("EXEMPLOS:\n");
    printf("  %s                                    # Configuração padrão\n", program_name);
    printf("  %s -c 20 -b 2 -s 3                   # 20 clientes, 2 barbeiros, 3 lugares no sofá\n", program_name);
    printf("  %s -t 500:2000 -p 200:800            # Tempos mais rápidos\n", program_name);
    printf("  %s -v 8 -a 50:3000                   # Alta variabilidade nas chegadas\n", program_name);
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
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    
    while ((c = getopt_long(argc, argv, "c:C:b:s:t:p:a:v:h", long_options, &option_index)) != -1) {
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
    
    // Inicializa gerador de números aleatórios com melhor seed
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand((unsigned int)(tv.tv_sec ^ tv.tv_usec ^ getpid()));
    
    // Inicializa filas
    sofa_queue = createQueue();
    payment_queue = createQueue();
    
    // Inicializa array de estados dos clientes
    customer_states = malloc(config.max_customers * sizeof(CustomerState));
    for (int i = 0; i < config.max_customers; i++) {
        customer_states[i].id = i + 1;
        customer_states[i].is_getting_haircut = 0;
        customer_states[i].haircut_done = 0;
        customer_states[i].is_paying = 0;
        customer_states[i].payment_done = 0;
        customer_states[i].seated_in_chair = 0;
    }
    
    logMessage("=== INICIANDO SIMULAÇÃO DA BARBEARIA DO HILZER ===");
    printf("Configurações: %d clientes máx, %d capacidade, %d barbeiros, %d lugares no sofá\n",
           config.max_customers, config.max_capacity, config.num_barbers, config.sofa_capacity);
    printf("Tempos: corte %d-%dms, pagamento %d-%dms, chegada %d-%dms\n",
           config.min_haircut_time, config.max_haircut_time, config.min_payment_time, config.max_payment_time,
           config.min_arrival_interval, config.max_arrival_interval);
    printf("Fator de variabilidade: %d/10\n", config.variability_factor);
    
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
    free(sofa_queue);
    free(payment_queue);
    free(customer_states);
    free(barber_threads);
    free(barber_ids);
    free(customer_threads);
    free(customer_ids);
    
    return 0;
}