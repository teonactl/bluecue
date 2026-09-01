import QtQuick

// Area centrale "vuota" tra le due colonne. Il disegno vero e proprio dei
// cavi di collegamento avviene sovrapponendo un Canvas a tutta la finestra
// (vedi Main.qml), perche' i cavi devono partire dalle righe reali nelle due
// ListView, che vivono in colonne diverse. Puramente spaziatore visivo (ne'
// freccia ne' testo, rimossi su richiesta esplicita dell'utente): il
// feedback "cavo in volo tratteggiato" durante il trascinamento e' gia'
// disegnato dal Canvas sopra a tutto, questo componente non deve aggiungere
// altro.
Rectangle {
    property bool linkingActive: false

    implicitWidth: 120
    color: "transparent"
}
