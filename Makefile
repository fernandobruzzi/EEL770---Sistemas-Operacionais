# Makefile para o Problema da Barbearia do Hilzer
# Configurações da simulação

# Parâmetros configuráveis
MAX_CUSTOMERS ?= 50
MAX_CAPACITY ?= 20
NUM_BARBERS ?= 3
SOFA_CAPACITY ?= 4
MIN_HAIRCUT_TIME ?= 1000
MAX_HAIRCUT_TIME ?= 5000
MIN_PAYMENT_TIME ?= 500
MAX_PAYMENT_TIME ?= 2000
MIN_ARRIVAL_TIME ?= 100
MAX_ARRIVAL_TIME ?= 2000
VARIABILITY_FACTOR ?= 5

# Detecção automática do sistema operacional e configuração do compilador
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CC = gcc
    LDFLAGS = -lpthread
else ifeq ($(UNAME_S),Darwin)
    CC = clang
    LDFLAGS = -pthread
else
    CC = gcc
    LDFLAGS = -lpthread
endif

CFLAGS = -Wall -Wextra -std=c99 -pthread
TARGET = barbershop
SOURCE = hilzer_barbershop_problem_copilot.c

# Definições de macros baseadas nos parâmetros
DEFINES = -DMAX_CUSTOMERS=$(MAX_CUSTOMERS) \
          -DMAX_CAPACITY=$(MAX_CAPACITY) \
          -DNUM_BARBERS=$(NUM_BARBERS) \
          -DSOFA_CAPACITY=$(SOFA_CAPACITY) \
          -DMIN_HAIRCUT_TIME=$(MIN_HAIRCUT_TIME) \
          -DMAX_HAIRCUT_TIME=$(MAX_HAIRCUT_TIME) \
          -DMIN_PAYMENT_TIME=$(MIN_PAYMENT_TIME) \
          -DMAX_PAYMENT_TIME=$(MAX_PAYMENT_TIME)

# Regra padrão
all: $(TARGET)

# Compilação do programa principal
$(TARGET): $(SOURCE)
	@echo "Compilando com as seguintes configurações:"
	@echo "  - Sistema: $(UNAME_S)"
	@echo "  - Compilador: $(CC)"
	@echo "  - Máximo de clientes: $(MAX_CUSTOMERS)"
	@echo "  - Capacidade da loja: $(MAX_CAPACITY)"
	@echo "  - Número de barbeiros: $(NUM_BARBERS)"
	@echo "  - Lugares no sofá: $(SOFA_CAPACITY)"
	@echo "  - Tempo de corte: $(MIN_HAIRCUT_TIME)-$(MAX_HAIRCUT_TIME)ms"
	@echo "  - Tempo de pagamento: $(MIN_PAYMENT_TIME)-$(MAX_PAYMENT_TIME)ms"
	@echo "  - Intervalo de chegada: $(MIN_ARRIVAL_TIME)-$(MAX_ARRIVAL_TIME)ms"
	@echo "  - Fator de variabilidade: $(VARIABILITY_FACTOR)/10"
	@echo ""
	$(CC) $(CFLAGS) $(DEFINES) -o $(TARGET) $(SOURCE) $(LDFLAGS)
	@echo "Compilação concluída! Execute com: ./$(TARGET)"

# Configurações predefinidas para diferentes cenários

# Cenário pequeno para testes rápidos
small: 
	$(MAKE) MAX_CUSTOMERS=10 MAX_CAPACITY=8 NUM_BARBERS=2 SOFA_CAPACITY=3

# Cenário padrão do livro
default:
	$(MAKE) MAX_CUSTOMERS=50 MAX_CAPACITY=20 NUM_BARBERS=3 SOFA_CAPACITY=4

# Cenário grande para stress test
large:
	$(MAKE) MAX_CUSTOMERS=100 MAX_CAPACITY=30 NUM_BARBERS=5 SOFA_CAPACITY=6

# Cenário rápido (tempos menores)
fast:
	$(MAKE) MIN_HAIRCUT_TIME=500 MAX_HAIRCUT_TIME=2000 MIN_PAYMENT_TIME=200 MAX_PAYMENT_TIME=800

# Cenário lento (tempos maiores)
slow:
	$(MAKE) MIN_HAIRCUT_TIME=3000 MAX_HAIRCUT_TIME=8000 MIN_PAYMENT_TIME=1000 MAX_PAYMENT_TIME=3000

# Cenário de alta variabilidade
variable:
	$(MAKE) VARIABILITY_FACTOR=8 MIN_ARRIVAL_TIME=50 MAX_ARRIVAL_TIME=3000

# Cenário de máxima variabilidade
chaos:
	$(MAKE) VARIABILITY_FACTOR=10 MIN_ARRIVAL_TIME=20 MAX_ARRIVAL_TIME=5000 MIN_HAIRCUT_TIME=500 MAX_HAIRCUT_TIME=8000

# Execução com diferentes configurações
run: $(TARGET)
	./$(TARGET)

run-small:
	$(MAKE) small
	./$(TARGET)

run-default:
	$(MAKE) default
	./$(TARGET)

run-large:
	$(MAKE) large
	./$(TARGET)

run-fast:
	$(MAKE) fast
	./$(TARGET)

run-slow:
	$(MAKE) slow
	./$(TARGET)

run-variable:
	$(MAKE) variable
	./$(TARGET)

run-chaos:
	$(MAKE) chaos
	./$(TARGET)

# Limpeza
clean:
	rm -f $(TARGET)
	@echo "Arquivos limpos!"

# Debug version
debug:
	$(CC) $(CFLAGS) $(DEFINES) -g -DDEBUG -o $(TARGET)_debug $(SOURCE) $(LDFLAGS)
	@echo "Versão debug compilada: $(TARGET)_debug"

# Regras de ajuda
help:
	@echo "Makefile para o Problema da Barbearia do Hilzer"
	@echo ""
	@echo "Uso básico:"
	@echo "  make              - Compila com configurações padrão"
	@echo "  make run          - Compila e executa"
	@echo "  make clean        - Remove arquivos compilados"
	@echo ""
	@echo "Configurações predefinidas:"
	@echo "  make small        - Cenário pequeno (10 clientes, 2 barbeiros)"
	@echo "  make default      - Cenário padrão do livro"
	@echo "  make large        - Cenário grande (100 clientes, 5 barbeiros)"
	@echo "  make fast         - Tempos de execução mais rápidos"
	@echo "  make slow         - Tempos de execução mais lentos"
	@echo ""
	@echo "Execução rápida:"
	@echo "  make run-small    - Compila e executa cenário pequeno"
	@echo "  make run-default  - Compila e executa cenário padrão do livro"
	@echo "  make run-large    - Compila e executa cenário grande"
	@echo "  make run-fast     - Compila e executa com tempos rápidos"
	@echo "  make run-slow     - Compila e executa com tempos lentos"
	@echo "  make run-variable - Compila e executa com alta variabilidade"
	@echo "  make run-chaos    - Compila e executa com máxima variabilidade"
	@echo ""
	@echo "Configurações de variabilidade:"
	@echo "  make variable     - Alta variabilidade nos tempos"
	@echo "  make chaos        - Máxima variabilidade (resultados muito diferentes)"
	@echo ""
	@echo "Personalização:"
	@echo "  make MAX_CUSTOMERS=30 NUM_BARBERS=4 - Configuração customizada"
	@echo ""
	@echo "Debug:"
	@echo "  make debug        - Compila versão debug"
	@echo ""
	@echo "Parâmetros disponíveis:"
	@echo "  MAX_CUSTOMERS     - Número total de clientes (padrão: 50)"
	@echo "  MAX_CAPACITY      - Capacidade máxima da loja (padrão: 20)"
	@echo "  NUM_BARBERS       - Número de barbeiros (padrão: 3)"
	@echo "  SOFA_CAPACITY     - Lugares no sofá (padrão: 4)"
	@echo "  MIN_HAIRCUT_TIME  - Tempo mínimo de corte em ms (padrão: 1000)"
	@echo "  MAX_HAIRCUT_TIME  - Tempo máximo de corte em ms (padrão: 5000)"
	@echo "  MIN_PAYMENT_TIME  - Tempo mínimo de pagamento em ms (padrão: 500)"
	@echo "  MAX_PAYMENT_TIME  - Tempo máximo de pagamento em ms (padrão: 2000)"
	@echo "  MIN_ARRIVAL_TIME  - Tempo mínimo entre chegadas em ms (padrão: 100)"
	@echo "  MAX_ARRIVAL_TIME  - Tempo máximo entre chegadas em ms (padrão: 2000)"
	@echo "  VARIABILITY_FACTOR- Fator de variabilidade 1-10 (padrão: 5)"

# Torna as regras como phony (não criam arquivos)
.PHONY: all clean run debug help small default large fast slow variable chaos run-small run-default run-large run-fast run-slow run-variable run-chaos
