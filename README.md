# Cemu+ Inspection

Cemu+ Inspection è un progetto di analisi e miglioramento tecnico di Cemu per Android, pensato per studiare il funzionamento interno dell'emulatore e individuare possibili ottimizzazioni in termini di prestazioni, compatibilità e stabilità.

Il progetto introduce un sistema di PPC Pre-Execution Analysis: un analizzatore che esamina il codice PowerPC dei giochi durante il caricamento, prima della sua esecuzione, raccogliendo informazioni sulle istruzioni e organizzandole in una cache consultabile.

### Obiettivi principali

- Analizzare preventivamente le sezioni di codice PowerPC eseguibili.
- Identificare le istruzioni supportate, non supportate o da approfondire.
- Creare una cache dei risultati per evitare analisi ripetute.
- Fornire informazioni utili per migliorare interpreter e JIT.
- Individuare potenziali problemi di compatibilità e stabilità.
- Misurare i miglioramenti con test e benchmark reali.

### Come funziona

1. Cemu carica normalmente il gioco.
2. L'Inspector individua le sezioni contenenti codice eseguibile.
3. Analizza le istruzioni PowerPC senza eseguirle.
4. Registra i risultati in una struttura indicizzata per indirizzo.
5. Salva i metadati in una cache associata al gioco e alla versione del sistema di analisi.
6. Le informazioni potranno essere consultate dai componenti dell'emulatore, mantenendo il comportamento originale come fallback.

### Filosofia del progetto

La priorità è migliorare Cemu senza compromettere la correttezza dell'emulazione. L'analisi iniziale sarà passiva: nessuna istruzione verrà modificata o saltata automaticamente. Eventuali ottimizzazioni saranno introdotte solo dopo verifiche tecniche e benchmark.
