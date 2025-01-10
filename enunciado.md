### Exercício 1. Interação com processos clientes por named pipes

O IST-KVS deve passar a ser um processo servidor autónomo, lançado da seguinte forma:
kvs **dir_jobs max_threads backups_max nome_do_FIFO_de_registo**
Quando se inicia, o servidor deve criar um _named pipe_ cujo nome ( _pathname_ ) é o indicado
como 4º argumento no comando acima. É através deste _pipe_ que os processos clientes se
poderão ligar ao servidor e enviar pedidos de início de sessão. O 1º argumento é idêntico à
primeira parte do projecto e indica a directoria onde o servidor deverá ler os ficheiros de
comandos a executar. O 2º argumento é o número máximo de tarefas a processar ficheiros
jobs e o 3º argumento indica o número máximo de backups concorrentes, tal como na 1ª
entrega do projeto. O cliente deverá ser lançado, com:
client **id_do_cliente nome_do_FIFO_de_registo**
Qualquer processo cliente pode ligar-se ao _pipe_ do servidor e enviar-lhe uma mensagem a
solicitar o início de uma sessão. Esse pedido contém os nomes de três _named pipe_ , que o
cliente previamente criou para a nova sessão. É através destes _named pipes_ que o cliente
enviará futuros pedidos para o servidor e receberá as correspondentes respostas do
servidor no âmbito da nova sessão (ver Figura 1).


O cliente deve criar o FIFO de notificações, o FIFO de pedido e o FIFO de resposta
adicionando ao nome dos pipes o id único recebido como argumento, como está no código
base.
O servidor aceita no máximo S sessões em simultâneo, em que S é uma constante definida
no código do servidor. Isto implica que o servidor, quando recebe um novo pedido de início
de sessão e tem S sessões ativas, deve bloquear, esperando que uma sessão termine para
que possa criar a nova.
Durante uma sessão, um cliente pode enviar comandos para adicionar ou remover
subscrições a chaves, usando os FIFOs de pedidos e de respostas. Enquanto a sessão
está ativa, o cliente irá obter atualizações para todas as chaves subscritas até aquele
momento através do FIFO de notificações.
Uma sessão dura até ao momento em que o servidor detecta que o cliente está
indisponível, ou o servidor recebe o SIGUSR1, como descrito no Ex.3. Nas subsecções
seguintes descrevemos a API cliente do IST-KVS em maior detalhe, assim como o conteúdo
das mensagens de pedido e resposta trocadas entre clientes e servidor.

### API cliente do IST-KVS

Para permitir que os processos cliente possam interagir com o IST-KVS, existe uma
interface de programação (API), em C, a qual designamos por API cliente do IST-KVS. Esta
API permite ao cliente ter programas que estabelecem uma sessão com um servidor e,
durante essa sessão, adicionar e remover subscrições a pares chave-valor da tabela de
dispersão gerida pelo IST-KVS. Quando um cliente subscreve uma dada chave, isso
significa que, sempre que haja uma alteração do valor dessa chave, esse cliente será
notificado do novo valor através de um pipe ligado ao servidor. De seguida apresentamos
essa API.
As seguintes operações permitem que o cliente estabeleça e termine uma sessão com o
servidor:
● int kvs_connect (char const *req_pipe_path, char const
*resp_pipe_path, char const *notifications_pipe_path, char
const *server_pipe_path)
Estabelece uma sessão usando os _named pipes_ criados.
Os _named pipes_ usados pela troca de pedidos e respostas e pela recepção das
notificações (isto é, após o estabelecimento da sessão) devem ser criados pelo
cliente (chamando _mkfifo)_. O _named pipe_ do servidor deve já estar previamente
criado pelo servidor, e o correspondente nome é passado como argumento inicial do
programa.
Retorna 0 em caso de sucesso, 1 em caso de erro.
● int kvs_disconnect()
Termina uma sessão ativa, envia o pedido de disconnect para o FIFO de pedido
identificada na variável respectiva do cliente, fechando os _named pipes_ que o cliente
tinha aberto quando a sessão foi estabelecida e apagando os _named pipes_ do
cliente. Deve ter o efeito no servidor de eliminar todas as subscrições de pares
chave-valor deste cliente.


Retorna 0 em caso de sucesso, 1 em caso de erro.
Tendo uma sessão ativa, o cliente pode invocar as seguintes operações junto do servidor:
● int kvs_subscribe(char const* key)
● int kvs_unsubscribe(char const* key)
A operação de kvs_subscribe permite a um cliente indicar ao servidor que pretende
acompanhar as alterações a uma chave e devolverá ao cliente através do FIFO de resposta
um caráter com o valor 0 ou 1 em que 0 indica que a chave dessa não existia na hashtable
e 1 indica que existia.
A operação de kvs_unsubscribe permite a um cliente indicar ao servidor que pretende
parar de acompanhar as alterações a uma chave e devolverá ao cliente através do FIFO de
resposta um caracter com o valor 0 ou 1, em que 0 indica que a subscrição existia e foi
removida, e 1 que a subscrição não existia.
Cada vez que um cliente recebe uma resposta a um seu comando (connect, disconnect,
subscribe, unsubscribe) pelo FIFO de resposta deve ser imprimida uma nova linha no seu
stdout com uma mensagem no seguinte formato:
“Server returned <response-code> for operation: <connect|disconnect|subscribe|unsubscribe>
Para além de gerir os pedidos acima, o servidor pode enviar para o FIFO de Notificações
(ver Figura 1) de qualquer cliente atualizações de chaves para as quais tenha acabado de
ser efetuada uma escrita. Pretende-se que seja a tarefa que efetua uma escrita na
hashtable a enviar a atualização aos clientes que sejam subscritores desse par chave-valor;
na primeira parte do projeto um esta tarefa escrevia para o ficheiro .out, agora escreverá
para o FIFO de Notificações.
Diferentes processos clientes podem existir concorrentemente, todos eles invocando a API
acima indicada (concorrentemente entre si). Por simplificação, devem ser assumidos estes
pressupostos:
● Cada processo cliente deverá usar duas tarefas. A tarefa principal lê do stdin os
comandos: (SUBSCRIBE, UNSUBSCRIBE, DELAY e DISCONNECT) e gere o envio
de pedidos para o servidor e a recepção das correspondentes respostas, cujo
formato é indicado a seguir. Uma segunda tarefa é responsável por receber
notificações e imprimir o resultado para o stdout. Para facilitar os testes da
aplicação, podem usar também o comando de DELAY X para atrasar a execução em
X segundos (isto é necessário pois entre os SUBSCRIBE e UNSUBSCRIBE vão
possivelmente querer adicionar um DELAY). O parser para estes comandos já está
disponibilizado no código base (ficheiro _parser.c_ ). O ficheiro test_client.txt inclui um
possível teste, que poderá ser executado redireccionando o std_input do client para
o ficheiro test_client.txt, como exemplificado a seguir:
./client/client c2 register_fifo < test_client.txt
● Os processos cliente são corretos, ou seja cumprem a especificação que é descrita
no resto deste documento. Contudo, podem desconectar-se de forma imprevista e
isto não deve pôr em risco a correção do processo servidor.
● Quando um cliente fecha os pipes que o ligam ao servidor e o servidor detecta esse
evento, o servidor deve eliminar todas as subscrições desse cliente.
Ao receber do servidor a notificação que uma certa chave mudou, o cliente deve imprimir no
seu _stdout_ uma mensagem no seguinte formato:


● (<chave>,<valor>)
,em que < _chave>_ é a chave alterada e < _valor>_ é o novo valor dessa chave.
Cada par (<chave>,<valor>) deve aparecer no terminal numa linha separada.
Caso uma chave subscrita seja apagada, deverá ser imprimido:
● (<chave>,DELETED)

### Protocolo de pedidos-respostas

O conteúdo de cada mensagem (de pedido e resposta) da API cliente deve seguir o
seguinte formato:


int kvs_connect(char const *req_pipe_path, char const* resp_pipe_path, char const
* _notifications_pipe_path,_ char const * _server_pipe_path_ )
Mensagens de pedido
(char) OP_CODE=1 | (char[40]) nome do pipe do cliente (para pedidos) | (char[40]) nome do pipe do cliente (para
respostas) | (char[40]) nome do pipe do cliente (para notificações)
Mensagens de resposta
(char) OP_CODE=1 | (char) result


int kvs_disconnect(void)
Mensagens de pedido
(char) OP_CODE=
Mensagens de resposta
(char) OP_CODE=2 | (char) result


int kvs_subscribe(char const* key)
Mensagens de pedido
(char) OP_CODE=3 | char[41] key
Mensagens de resposta
(char) OP_CODE=3 | (char) result



int kvs_unsubscribe(char const* key)
Mensagens de pedido e resposta
(char) OP_CODE=4 | (char) [41] key
Mensagens de resposta
(char) OP_CODE=4 | (char) result


Onde:
● O símbolo | denota a concatenação de elementos numa mensagem. Por exemplo, a
mensagem de resposta associada à função kvs_disconnect consiste num _byte_
( char ) seguido de um outro _byte_ ( char ).
● Todas as mensagens de pedido são iniciadas por um código que identifica a
operação solicitada ( OP_CODE ).
● As _strings_ são de tamanho fixo (40 caracteres). No caso de nomes de tamanho
inferior, os caracteres adicionais devem ser preenchidos com ‘ \0 ’.
● As mensagens que o servidor envia aos clientes através do FIFO de notificações
correspondente a esse cliente cada vez que há uma actualização de uma chave
deverão consistir de 2 vetores de 41 caracteres (40 para o nome da chave, 1 para o
caráter de terminação do nome da chave, 40 para o valor e 1 para o caráter de
terminação do valor, caso a chave ou o valor não sejam de 40 caracteres, deverá ser
adicionado padding para tal).
● O caso no qual o _byte_ ( char ) _result_ tem valor igual a 0 indica sucesso, qualquer
outro valor indica um problema na execução de _kvs_connect_ ou _kvs_disconnect_.


**Figura 1: Arquitetura do IST-KVS (a verde o processo de registo, a azul o processo de pedido e
resposta, a laranja o processo de notificações, a vermelho a leitura dos ficheiros .job).**


### Implementação em duas etapas

Dada a complexidade deste requisito, recomenda-se que a solução seja desenvolvida de
forma gradual, em 2 etapas que descrevemos de seguida.

## Etapa 1.1: Servidor IST-KVS com sessão única

Nesta fase, devem ser assumidas as seguintes simplificações (que serão eliminadas no
próximo requisito):
● O servidor processa os ficheiros com os jobs como na primeira parte do projecto
mas usa uma única tarefa adicional para atender apenas um cliente remoto de cada
vez.
_●_ O servidor só aceita uma sessão de cada vez (ou seja, S=1).
**Experimentem:**
Corra o teste disponibilizado em jobs/test.job sobre a sua implementação
cliente-servidor do IST-KVS. Confirme que o teste termina com sucesso.
Construa e experimente testes mais elaborados que exploram diferentes funcionalidades
oferecidas pelo servidor IST-KVS.

## Etapa 1.2: Suporte a múltiplas sessões concorrentes

Nesta etapa, a solução composta até ao momento deve ser estendida para suportar os
seguintes aspetos mais avançados.
Por um lado, o servidor deve passar a suportar múltiplas sessões ativas em simultâneo (ou
seja, S>1).
Por outro lado, o servidor deve ser capaz de tratar pedidos de sessões distintas (ou seja, de
clientes distintos) em paralelo, usando múltiplas tarefas ( _threads)_ , entre as quais:
● Uma das tarefas do servidor deve ficar responsável por receber os pedidos que
chegam ao servidor através do seu _pipe_ , sendo por isso chamada a _tarefa anfitriã_.
● Existem também S tarefas gestoras que gerem os pedidos de subscrição, cada uma
associada a um cliente e dedicada a servir os pedidos do cliente correspondente a
esta sessão_._ As tarefas gestoras devem ser criadas aquando da inicialização do
servidor. Note-se que estas tarefas são independentes das necessárias para ler os
ficheiros com comandos e executá-los.
A tarefa anfitriã coordena-se com as tarefas que gerem os pedidos de subscrição da
seguinte forma:
● Quando a tarefa anfitriã recebe um pedido de estabelecimento de sessão por um
cliente, a tarefa anfitriã insere o pedido num buffer produtor-consumidor. As tarefas
gestoras extraem pedidos deste _buffer_ e comunicam com o respectivo cliente
através dos _named pipes_ que o cliente terá previamente criado e comunicado junto
ao pedido de estabelecimento da sessão. A sincronização do _buffer_
produtor-consumidor deve basear-se em semáforos (além de _mutexes_ ).


```
Experimente:
Experimente correr os testes cliente-servidor que compôs anteriormente, mas agora
lançando-os concorrentemente por 2 ou mais processos cliente.
```
### Exercício 2. Terminação das ligações aos clientes usando sinais

Estender o IST-KVS de forma que no servidor seja redefinida a rotina de tratamento do sinal
SIGUSR1. Ao receber este sinal, o (servidor) IST-KVS deve memorizar (na rotina de
tratamento do sinal) que recebeu um SIGUSR.
Apenas a tarefa anfitriã que recebe as ligações de registo de clientes deve escutar o
SIGUSR1. Dado que só a tarefa anfitriã recebe o sinal, todas as tarefas de atendimento de
um cliente específico devem usar a função pthread_sigmask para inibirem (com a opção
SIG_BLOCK ) a recepção do SIGUSR.
Tendo recebido um SIGUSR1, a tarefa anfitriã eliminará todas as subscrições existentes na
_hashtable_ e encerrará os FIFOs de notificação e de resposta para todos os clientes, isto fará
com que os clientes terminem.
As tarefas ligadas aos clientes deverão lidar com o erro associado a tentar escrever ou ler
de um pipe que agora estará fechado (pois a tarefa anfitriã fechou todos estes pipes), e ao
se dar este erro devem esperar novamente uma conexão do próximo cliente que se tente
conectar com _kvs_connect_ , querendo com isto dizer que SIGUSR1 não desliga o servidor,
apenas elimina as subscrições da hashtable e desconecta todos os clientes,
desassociando-os das tarefas a que estão associados, estas tarefas vão depois esperar os
próximos clientes.
Quando os pipes do servidor fecharem ao receber o sinal, o cliente deverá terminar.


# Arquitetura do Sistema IST-KVS

Este documento descreve a arquitetura representada na imagem, detalhando os componentes, fluxos de operação e sugestões de implementação para um sistema em C.

---

## **Componentes do Sistema**

### **1. Ficheiros pasta jobs**
- Representado por um ícone de base de dados no canto superior direito.
- Armazena os ficheiros associados às tarefas.
- Fonte de dados inicial para o processamento do sistema.

### **2. Pool de Tarefas que Lêem Ficheiros**
- Conjunto de threads responsáveis por ler os ficheiros armazenados na "Ficheiros pasta jobs".
- Processa os dados iniciais e os repassa para as outras camadas do sistema.
- Cada tarefa neste pool funciona como um thread independente.

### **3. Tarefa Anfitriã**
- Representada por um bloco verde no centro do diagrama.
- Atua como controlador principal, coordenando interações entre:
  - **Pool de Tarefas que Lêem Ficheiros**
  - **Pool de Tarefas Gestoras**
  - As filas de comunicação (FIFOs).

### **4. Pool de Tarefas Gestoras**
- Representado por um bloco azul.
- Conjunto de threads responsáveis pela execução das operações principais.
- Realiza o processamento dos pedidos recebidos pelos clientes de forma assíncrona e simultânea.
- Cada thread neste pool fica responsável por atender aos pedidos (e.g., `subscribe`, `unsubscribe`, `disconnect`) de um cliente que se tenha registado:
  - Lendo da sua **FIFO de Pedido**.
  - Escrevendo na sua **FIFO de Resposta**.
- O número total de threads gestoras será igual ao número máximo de sessões simultâneas que o servidor permite. Por exemplo:
  - Caso seja imposto um limite, pode-se configurar até 8 threads gestoras.

---

## **Estrutura de Comunicação (FIFOs)**

### **1. FIFO de Pedido**
- Cada cliente se comunica com o sistema através de uma FIFO de pedido.
- Os pedidos são enviados para o **Pool de Tarefas Gestoras** para processamento.

### **2. FIFO de Resposta**
- As respostas dos pedidos processados são enviadas de volta aos clientes por FIFOs dedicadas.

### **3. FIFO de Notificações**
- Canal para as tarefas notificarem o sistema ou os clientes sobre:
  - Progresso
  - Conclusão de tarefas

### **4. FIFO de Registo**
- Representado por um bloco verde na lateral do diagrama.
- Usado para registrar informações do sistema, como:
  - Logs das tarefas
  - Dados de depuração

---

## **Fluxo de Operações**

1. **Entrada de Dados**:
   - Os ficheiros são lidos pela **Pool de Tarefas que Lêem Ficheiros**.
   - Os dados lidos são extraídos e enviados para a **Tarefa Anfitriã**.

2. **Distribuição de Tarefas**:
   - A **Tarefa Anfitriã** distribui os dados para o **Pool de Tarefas Gestoras**.

3. **Processamento e Respostas**:
   - Os clientes enviam pedidos ao sistema através das **FIFOs de Pedido**.
   - As tarefas gestoras processam os pedidos e enviam as respostas via **FIFOs de Resposta**.

4. **Notificações**:
   - Informações sobre progresso ou conclusão das tarefas são enviadas através das **FIFOs de Notificações**.

5. **Registo**:
   - Logs e informações de depuração são armazenados na **FIFO de Registo**.

---

## **Estrutura do Cliente**

- O cliente utiliza **apenas duas threads** para comunicação e processamento:
  1. **Thread 1**:
     - Lê comandos do `stdin`.
     - Envia os comandos ao servidor.
     - Recebe a resposta e apresenta o resultado no `stdout`.
  2. **Thread 2**:
     - Lê da **FIFO de Notificações**.
     - Imprime as notificações recebidas no `stdout`.

---

## **Sugestões para Implementação em C**

### **1. Threads**
- Utilize `pthread` para implementar:
  - **Pool de Tarefas que Lêem Ficheiros**
  - **Pool de Tarefas Gestoras**
  - Threads específicas no cliente.

### **2. Comunicação com FIFOs**
- Use named pipes (`mkfifo`) para implementar as filas de comunicação (FIFOs).

### **3. Sincronização**
- Utilize mutexes e semáforos (`pthread_mutex` e `sem_t`) para sincronizar o acesso às FIFOs e evitar condições de corrida.

### **4. Estruturas de Dados**
- Defina estruturas claras para:
  - **Pedidos**
  - **Respostas**
  - **Notificações**
- Certifique-se de que as FIFOs manipulem os dados de maneira consistente.

### **5. Logs e FIFO de Registo**
- Configure uma thread dedicada para processar os logs enviados à FIFO de Registo.

### **6. Configuração do Pool de Tarefas Gestoras**
- Limite o número de threads gestoras ao máximo de sessões simultâneas permitidas.
- Considere um limite prático, como 8 threads gestoras.

---

IMPORTANTE:

o server/kvs. tem de ter 2 pools de tarefas, uma pool com varias tarefas que leem e processam os ficheiros .job na pasta jobs dada. Na outra pool de tarefas(Pool de Tarefas gestoras), Cada thread neste conjunto fica responsável por atender aos pedidos (subscribe, unsubscribe e disconnect) de um cliente que se tenha registado, o que implica ler da sua FIFO de pedido e escrever na sua FIFO de resposta.
Isto significa que o número total de threads gestoras será igual ao número máximo de sessões que o teu servidor vai permitir simultaneamente.

A ideia é que o cliente utilize apenas duas threads. Uma lê do stdin os comandos, envia-os ao servidor e recebe a resposta. A outra é responsável por ler da fifo de notificações e por printar as notificações para o stdout.
