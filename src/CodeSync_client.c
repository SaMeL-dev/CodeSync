// client.c
// CodeSync Client

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define BUF_SIZE 4096
#define MAX_MSG_LEN 10240

// 연결 리스트 구조체 정의: 커밋 히스토리 관리를 위함
typedef struct CommitNode {
    char data[1024];
    struct CommitNode* next;
} CommitNode;

// 함수 선언
unsigned WINAPI RecvMsg(void* arg);             // 서버로부터 메시지 수신하는 스레드
void ErrorHandling(char* msg);                  // 에러 출력 및 종료
void DisplayHistory(char* historyData);         // 커밋 히스토리 출력
void FreeHistory(CommitNode* head);             // 히스토리 메모리 해제

char name[30] = "[DEFAULT]";
char msg[MAX_MSG_LEN];
char currentProject[50] = "";                   // 현재 작업 중인 프로젝트 이름

int main(int argc, char* argv[]) {
    WSADATA wsaData;
    SOCKET hSock;
    SOCKADDR_IN servAdr;
    HANDLE hRecvThread;
    char id[30], pw[30], servMsg[BUF_SIZE];
    int strLen;

    if (argc != 3) {
        printf("사용법: %s <서버 IP> <포트번호>\n", argv[0]);
        exit(1);
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        ErrorHandling("WSAStartup() error");

    hSock = socket(PF_INET, SOCK_STREAM, 0);
    if (hSock == INVALID_SOCKET)
        ErrorHandling("socket() error");
    
    // 서버 주소 설정
    memset(&servAdr, 0, sizeof(servAdr));
    servAdr.sin_family = AF_INET;
    servAdr.sin_addr.s_addr = inet_addr(argv[1]);
    servAdr.sin_port = htons(atoi(argv[2]));

    if (connect(hSock, (SOCKADDR*)&servAdr, sizeof(servAdr)) == SOCKET_ERROR)
        ErrorHandling("connect() 함수 오류");
    
    // 로그인 입력
    printf("ID: ");
    fgets(id, sizeof(id), stdin);
    id[strcspn(id, "\n")] = 0; // 개행 제거
    printf("PW: ");
    fgets(pw, sizeof(pw), stdin);
    pw[strcspn(pw, "\n")] = 0;
    
    // 로그인 메시지 전송
    sprintf(msg, "%s\n%s\n##END##\n", id, pw);
    send(hSock, msg, strlen(msg), 0);
    
    strLen = recv(hSock, servMsg, BUF_SIZE - 1, 0);
    servMsg[strLen] = 0;
    
    // 로그인 결과 확인
    if (strstr(servMsg, "LOGIN_SUCCESS")) {
        printf("로그인 성공\n");
        strcpy(name, id);
    } else {
        printf("로그인 실패\n");
        closesocket(hSock);
        return 1;
    }

    // 수신 스레드 생성
    hRecvThread = (HANDLE)_beginthreadex(NULL, 0, RecvMsg, (void*)&hSock, 0, NULL);
    
    // 프로젝트 선택 / 생성
    printf("참여 또는 생성할 프로젝트 이름 입력: ");
    fgets(currentProject, sizeof(currentProject), stdin);
    currentProject[strcspn(currentProject, "\n")] = 0;
    
    sprintf(msg, "create_project\n%s\n##END##\n", currentProject);
    send(hSock, msg, strlen(msg), 0);

    Sleep(1000);  // 시스템 답변을 받을 시간 1초 대기

    // 명령어 루프
    while (1) {
        fgets(msg, BUF_SIZE, stdin);
        msg[strcspn(msg, "\n")] = 0;
        
        if (strcmp(msg, "exit") == 0) {
            break;
        } else if (strcmp(msg, "commit") == 0) {
            char title[100], commitMsg[512], code[8192], temp[1024];
            printf("커밋 제목: ");
            fgets(title, sizeof(title), stdin);
            title[strcspn(title, "\n")] = 0;
            
            printf("커밋 메시지: ");
            fgets(commitMsg, sizeof(commitMsg), stdin);
            commitMsg[strcspn(commitMsg, "\n")] = 0;

            printf("코드 입력 (마지막 줄에 '##CODE_END##' 입력):\n");
            strcpy(code, "");
            while(fgets(temp, sizeof(temp), stdin)) {
                if (strcmp(temp, "##CODE_END##\n") == 0) break;
                strcat(code, temp);
            }
            sprintf(msg, "commit\n%s\n%s\n%s\n%s##END##\n", currentProject, title, commitMsg, code);
            send(hSock, msg, strlen(msg), 0);

        } else if (strcmp(msg, "history") == 0) {
            sprintf(msg, "history\n%s\n##END##\n", currentProject);
            send(hSock, msg, strlen(msg), 0);
        } else if (strcmp(msg, "revoke") == 0) {
            char commitID[100];
            char msg[MAX_MSG_LEN];

            printf("취소할 CommitID 입력: ");
            fgets(commitID, sizeof(commitID), stdin);
            commitID[strcspn(commitID, "\n")] = 0;

            sprintf(msg, "revoke\n%s\n%s\n##END##\n", currentProject, commitID);
            send(hSock, msg, strlen(msg), 0);
            // char title[100];
            // printf("취소할 커밋 ID: ");
            // fgets(title, sizeof(title), stdin);
            // title[strcspn(title, "\n")] = 0;

            // sprintf(msg, "revoke\n%s\n%s\n##END##\n", currentProject, title);
            // send(hSock, msg, strlen(msg), 0);
        } else {
            printf("잘못된 명령어입니다.\n");
        }
    }

    closesocket(hSock);
    WaitForSingleObject(hRecvThread, INFINITE);
    CloseHandle(hRecvThread);
    WSACleanup();
    return 0;
}

// 서버로부터 수신되는 메시지를 처리하는 스레드 함수
unsigned WINAPI RecvMsg(void* arg) {
    SOCKET hSock = *((SOCKET*)arg);
    char servMsg[MAX_MSG_LEN];
    int strLen;

    while ((strLen = recv(hSock, servMsg, MAX_MSG_LEN - 1, 0)) > 0) {
        servMsg[strLen] = 0;
        
        char* token_end = strstr(servMsg, "##END##");
        if(token_end) *token_end = '\0';
        
        if (strncmp(servMsg, "UPDATE", 6) == 0) {
            char* projectName = strtok(servMsg, "\n"); // "UPDATE"
            projectName = strtok(NULL, "\n"); // 프로젝트 이름
            if (projectName && strcmp(projectName, currentProject) == 0) {
                printf("\n[알림] 이 프로젝트에 새로운 커밋이 발생했습니다.\n");
                printf("로그를 새로고침하려면 'history' 명령어를 입력하세요.\n");
                fflush(stdout);
            }
        } else if (strstr(servMsg, "PROJECT_CREATED")) {
            printf("[시스템] 프로젝트 '%s' 생성 완료.\n", currentProject);
            Sleep(1000);
            printf("\n(Project: %s) 명령 입력 (commit, history, revoke, exit): ", currentProject);
        } else if (strstr(servMsg, "PROJECT_EXISTS")) {
            printf("[시스템] 기존 프로젝트 '%s'에 참여했습니다.\n", currentProject);
            Sleep(1000);
            printf("\n(Project: %s) 명령 입력 (commit, history, revoke, exit): ", currentProject);
        } else if (strstr(servMsg, "COMMIT_SUCCESS")) {
            printf("[시스템] 커밋 성공.\n");
            Sleep(1000);
            printf("\n(Project: %s) 명령 입력 (commit, history, revoke, exit): ", currentProject);
        } else if (strstr(servMsg, "REVOKE_SUCCESS")) {
            printf("[시스템] 커밋 취소 성공.\n");
            Sleep(1000);
            printf("\n(Project: %s) 명령 입력 (commit, history, revoke, exit): ", currentProject);
        } else if (strstr(servMsg, "REVOKE_FAIL")) {
            printf("[시스템] 커밋 취소 실패 (해당 커밋이 없거나 오류 발생).\n");
            Sleep(1000);
            printf("\n(Project: %s) 명령 입력 (commit, history, revoke, exit): ", currentProject);
        } else if (strstr(servMsg, "INVALID_COMMAND")) {
            printf("[시스템] 서버가 잘못된 명령어를 수신했습니다.\n");
            Sleep(1000);
            printf("\n(Project: %s) 명령 입력 (commit, history, revoke, exit): ", currentProject);
        } else {
             DisplayHistory(servMsg);   // 커밋 히스토리 출력
             Sleep(1000);
             printf("\n(Project: %s) 명령 입력 (commit, history, revoke, exit): ", currentProject);
        }
    }
    printf("서버 연결이 종료되었습니다.\n");
    return 0;
}

// 연결 리스트를 사용한 히스토리 출력 함수
void DisplayHistory(char* historyData) {
    printf("\n--- %s 프로젝트 커밋 히스토리 ---\n", currentProject);
    
    if (strstr(historyData, "이 프로젝트에 대한 히스토리가 없습니다.")) {
        printf("%s\n", historyData);
        return;
    }

    CommitNode* head = NULL;
    CommitNode* tail = NULL;

    char* line = strtok(historyData, "\n");

    while (line != NULL) {
        CommitNode* newNode = (CommitNode*)malloc(sizeof(CommitNode));
        if (!newNode) {
            ErrorHandling("DisplayHistory에서 메모리 할당 오류");
            FreeHistory(head);
            return;
        }
        strncpy(newNode->data, line, sizeof(newNode->data) - 1);
        newNode->data[sizeof(newNode->data) - 1] = '\0';
        newNode->next = NULL;

        if (head == NULL) {
            head = newNode;
            tail = newNode;
        } else {
            tail->next = newNode;
            tail = newNode;
        }
        line = strtok(NULL, "\n");
    }
    
    CommitNode* current = head;
    while (current != NULL) {
        printf("%s\n", current->data);
        current = current->next;
    }

    printf("--- 히스토리 끝 ---\n");
    FreeHistory(head);
}

// 히스토리 메모리 해제
void FreeHistory(CommitNode* head) {
    CommitNode* tmp;
    while (head != NULL) {
        tmp = head;
        head = head->next;
        free(tmp);
    }
}

// 에러 메시지 출력 및 종료
void ErrorHandling(char* msg) {
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}