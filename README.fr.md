# drakonia-linux

Régler la souris **Sharkoon Drakonia** sans le logiciel du fabricant, sous **Linux et
Windows**. [English version](README.md)

- `drakonia.py` : un petit outil en ligne de commande (Python, marche sur les deux systèmes)
- `libratbag/` : un pilote pour [libratbag](https://github.com/libratbag/libratbag), pour
  régler la souris dans [Piper](https://github.com/libratbag/piper), le logiciel de souris
  graphique sous Linux
- `PROTOCOL.md` : le protocole USB complet, retrouvé à partir de captures (en anglais)

Tout ce que le logiciel officiel règle dans la souris est pris en charge : couleur, effet
et intensité du logo, DPI, fréquence, les 11 boutons et les macros.

## État

| | |
|---|---|
| `drakonia.py` | testé sur une vraie souris (sous Windows) |
| pilote libratbag | compile, produit les mêmes octets que le script testé, **pas encore testé sur la souris** |

## L'outil en ligne de commande

Il faut Python 3 et hidapi :

```
pip install hidapi
```

- **Windows** : fermer d'abord le logiciel Sharkoon (icône près de l'horloge).
- **Linux** : lancer avec `sudo`, ou installer une fois la règle udev pour que ton
  utilisateur ait accès à la souris :
  ```
  sudo cp 99-sharkoon-drakonia.rules /etc/udev/rules.d/
  sudo udevadm control --reload && sudo udevadm trigger
  ```

Exemples :

```
python drakonia.py selftest              # vérifie le codage, ne touche à rien
python drakonia.py color 0 0 255         # logo bleu
python drakonia.py lighting steady high  # steady|slow|medium|fast  off|low|medium|high
python drakonia.py dpi 2000              # 500 1000 1500 2000 3000 4000 5000
python drakonia.py rate 1000             # 125 250 500 1000 Hz
python drakonia.py sensitivity 5         # 1 (lent) .. 10 (usine) ; 2e valeur facultative pour Y
python drakonia.py buttons               # liste des boutons et des actions
python drakonia.py factory               # boutons et macros d'usine
python drakonia.py rescue                # si le pointeur ne bouge plus : retour au mode normal
python drakonia.py --dry-run dpi 2000    # affiche les octets, n'envoie rien
```

`steady` = fixe, `slow`/`medium`/`fast` = pulsation lente/moyenne/rapide ;
`off`/`low`/`medium`/`high` = éteint/faible/moyenne/forte.

### Boutons et macros

La souris reçoit toujours **tous les boutons et toutes les macros d'un coup**, et on ne
peut pas relire sa configuration. Il faut donc décrire toute ta configuration, dans un
fichier ou dans la commande ; les boutons non cités reprennent leur action d'usine.

```
python drakonia.py config --azerty "10=keys ctrl+c" "11=keys ctrl+v" "6=text bonjour"
python drakonia.py config --azerty --file examples/my-mouse.txt
```

| N° | Bouton | N° | Bouton |
|---|---|---|---|
| 1 | clic gauche | 7 | molette inclinée à droite |
| 2 | clic droit | 8 | DPI+ |
| 3 | clic molette | 9 | DPI− |
| 4 | suivant | 10 | bouton latéral droit |
| 5 | précédent | 11 | bouton latéral gauche |
| 6 | molette inclinée à gauche | | |

Actions : `left` (clic gauche), `right` (clic droit), `middle` (clic molette), `back`
(précédent), `forward` (suivant), `tilt-left`/`tilt-right` (molette inclinée), `dpi+`,
`dpi-`, `disable` (désactivé), `double-click`, `color-cycle` (couleur suivante),
`keys <raccourci>` (ex. `keys ctrl+shift+t`), `text <texte>`.

Les touches correspondent à des positions sur le clavier. Avec un clavier AZERTY, ajoute
`--azerty` pour que `text bonjour` tape bien « bonjour ».

## Le pilote libratbag (Piper)

Le pilote se trouve dans `libratbag/`, sous forme de correctif (patch) pour libratbag,
avec les deux nouveaux fichiers.

```
git clone https://github.com/libratbag/libratbag
cd libratbag
git am /chemin/vers/drakonia-linux/libratbag/0001-Add-a-driver-for-the-Sharkoon-Drakonia.patch
meson setup builddir && ninja -C builddir && sudo ninja -C builddir install
sudo systemctl restart ratbagd
```

Puis ouvrir Piper. À savoir :

- La souris ne peut pas être relue : au démarrage, Piper affiche la configuration
  d'usine, pas celle enregistrée dans la souris.
- L'action d'usine du bouton latéral droit (couleur suivante) n'existe pas dans
  libratbag : elle s'affiche comme « inconnue » et reste en place tant qu'on n'y touche pas.
- Les délais dans les macros sont ignorés (leur codage est inconnu).

Une fois testé sur la vraie souris, le pilote sera proposé à libratbag.

## Pas fait / inconnu

- relire la configuration enregistrée dans la souris
- les délais dans les macros
- le rôle du message `03` (voir `PROTOCOL.md`)
- la sensibilité n'est pas dans le pilote libratbag (Piper n'a pas ce réglage)
- vitesse du pointeur, du défilement et du double-clic : ce sont des réglages de Windows/KDE, pas de la souris

Les retours et captures d'autres possesseurs de Drakonia sont les bienvenus.

## Mentions légales

Projet sans lien avec Sharkoon. « Sharkoon » et « Drakonia » sont des marques de leurs
propriétaires. Le protocole a été étudié uniquement pour l'interopérabilité ; aucun code
ni fichier du fabricant n'est inclus dans ce dépôt.

Licence MIT, voir [LICENSE](LICENSE).
