#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

// --------- Tipos e recursos ---------
typedef struct {
    int id;
    int valor;
} dado_t;

static QueueHandle_t fila = NULL;                 // fila de ponteiros para dado_t
static EventGroupHandle_t event_supervisor = NULL;

// Bits do supervisor
#define BIT_TASK1_OK (1 << 0)
#define BIT_TASK2_OK (1 << 1)

// Prefixo exigido no enunciado
#define LOG_PREFIX "{William Huang-RM:87382}"

// --------- Task 1: Geração de dados (sequenciais) ---------
static void Task1(void *pv)
{
    int seq = 1;  // inteiro sequencial contínuo

    for(;;)
    {
        // aloca o pacote que será enviado
        dado_t *dados = (dado_t *) malloc(sizeof(dado_t));
        if (dados == NULL) {
            printf(LOG_PREFIX " [ERRO] Falha na alocação de memória na Task1!\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // gera inteiros sequenciais (id e valor)
        dados->id = seq++;
        dados->valor = dados->id; // valor também sequencial, conforme requisito

        // tenta enviar para a fila; se cheia, descarta e segue
        if (xQueueSend(fila, &dados, 0) != pdTRUE) {
            printf(LOG_PREFIX " [FILA] Fila cheia! Valor %d (ID %d) descartado\n", dados->valor, dados->id);
            free(dados);
        } else {
            xEventGroupSetBits(event_supervisor, BIT_TASK1_OK);
            printf(LOG_PREFIX " [TX] Valor %d (ID %d) enviado com sucesso\n", dados->valor, dados->id);
        }

        // alimenta WDT e espera um pouco
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --------- Task 2: Recepção (usa malloc/free no receptor) ---------
static void Task2(void *pv)
{
    dado_t *dados_recebidos = NULL;
    int timeout = 0;

    for(;;)
    {
        // espera até 1000 ms por dados
        if (xQueueReceive(fila, &dados_recebidos, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            // *** requisito: usar malloc/free no MÓDULO DE RECEPÇÃO ***
            // faz uma cópia dinâmica, processa, e libera
            dado_t *tmp = (dado_t *) malloc(sizeof(dado_t));
            if (tmp == NULL) {
                printf(LOG_PREFIX " [ERRO] Falha na alocação no receptor!\n");
            } else {
                *tmp = *dados_recebidos; // copia conteúdo
                printf(LOG_PREFIX " [RX] Valor %d (ID %d) recebido com sucesso\n", tmp->valor, tmp->id);
                free(tmp);
            }

            // libera o item original vindo da fila
            if (dados_recebidos != NULL) {
                free(dados_recebidos);
                dados_recebidos = NULL;
            }

            // sinaliza ok para o supervisor
            xEventGroupSetBits(event_supervisor, BIT_TASK2_OK);
            timeout = 0;
            esp_task_wdt_reset();
        }
        else
        {
            // não recebeu no prazo -> escalonamento
            timeout++;
            printf(LOG_PREFIX " [FILA] Nenhum dado recebido (%d tentativa%s)\n",
                   timeout, (timeout == 1 ? "" : "s"));

            if (timeout == 3) {
                printf(LOG_PREFIX " [ALERTA] Task2 com falhas na recepção!\n");
            }

            if (timeout >= 5) {
                // tentativa de recuperação (ação concreta: resetar fila)
                printf(LOG_PREFIX " [RECUPERAÇÃO] Reinicializando estado da fila...\n");
                if (fila) xQueueReset(fila);
                timeout = 0;
            }
        }
    }
}

// --------- Task 3: Supervisão (flags/status) ---------
static void Task3(void *pv)
{
    for(;;)
    {
        EventBits_t bits = xEventGroupWaitBits(
            event_supervisor,
            BIT_TASK1_OK | BIT_TASK2_OK,
            pdTRUE,             // limpa os bits lidos
            pdFALSE,            // não precisa de todos ao mesmo tempo
            pdMS_TO_TICKS(2000) // janela de observação
        );

        if ((bits & BIT_TASK1_OK) && (bits & BIT_TASK2_OK)) {
            printf(LOG_PREFIX " [SUP] Sistema OK (Task1 e Task2 ativas)\n");
        }
        else if (bits & BIT_TASK1_OK) {
            printf(LOG_PREFIX " [SUP] Sistema parcialmente OK (apenas Task1 sinalizou)\n");
        }
        else if (bits & BIT_TASK2_OK) {
            printf(LOG_PREFIX " [SUP] Sistema parcialmente OK (apenas Task2 sinalizou)\n");
        }
        else {
            printf(LOG_PREFIX " [FALHA] Nenhuma task sinalizou no intervalo!\n");
        }

        // alimenta WDT e espera
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// --------- app_main: inicialização, WDT e criação de tasks ---------
void app_main(void)
{
    printf(LOG_PREFIX " Iniciando Sistema de Dados Robusto (CP2)...\n");

    // Configura Watchdog para as tasks
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 5000,          // 5s
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_cfg);

    // Fila de tamanho 1 (força backpressure e testes de descarte); armazena ponteiros para dado_t
    fila = xQueueCreate(1, sizeof(dado_t *));
    event_supervisor = xEventGroupCreate();

    if (fila == NULL || event_supervisor == NULL) {
        printf(LOG_PREFIX " [ERRO] Falha na criação dos recursos (fila/event group)!\n");
        // falha crítica -> reinicializa dispositivo
        esp_restart();
    }

    // Criação das Tasks
    TaskHandle_t hTask1 = NULL, hTask2 = NULL, hTask3 = NULL;
    xTaskCreate(Task1, "Task1_Geracao",   4096, NULL, 5, &hTask1);
    xTaskCreate(Task2, "Task2_Recepcao",  4096, NULL, 5, &hTask2);
    xTaskCreate(Task3, "Task3_Supervisao",4096, NULL, 5, &hTask3);

    // Vincula as tasks ao WDT
    if (hTask1) esp_task_wdt_add(hTask1);
    if (hTask2) esp_task_wdt_add(hTask2);
    if (hTask3) esp_task_wdt_add(hTask3);
}
