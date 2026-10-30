#include "ui/settings_i18n.hpp"

#include <iterator>

namespace wowee {
namespace ui {

namespace {

// Every row of the schema, in German, in schema order.
//
// Where the original client has the same option, gsLabel and gsTooltip name
// its own GlobalStrings entries and the interface's text is taken when the
// interface is loaded. The label here is then that same text, so that the
// login screen - where there is no interface yet - says what the game says.
//
// Choices keep the schema's count and order: the index is what is saved.
constexpr SettingGerman kGerman[] = {
    {"graphicspreset",
     "Qualitätsstufe",
     "Stellt alle Grafikoptionen auf einmal ein: Sichtweite, Schatten, Kantenglättung, "
     "Oberflächendetails, Bodenbewuchs und Gras. Wird danach eine davon geändert, steht hier "
     "Benutzerdefiniert.",
     "Benutzerdefiniert|Niedrig|Mittel|Hoch|Ultra", nullptr, nullptr},
    {"viewdistance",
     "Sichtweite",
     "Wie weit die Welt in die Ferne gezeichnet wird, in Metern. Der größte einzelne Kostenpunkt "
     "des Bildes: Gelände, Gebäude und Kreaturen werden alle bis zu dieser Weite gezeichnet.",
     nullptr, "FARCLIP", nullptr},
    {"fogstrength",
     "Nebeldichte",
     "Wie dicht der Fernnebel ist, gemessen an der Gestaltung der Zone. 1 ist wie vorgesehen. "
     "Unter 1 wird er dünner, 0 entfernt ihn; über 1 rückt er näher.",
     nullptr, nullptr, nullptr},
    {"fogskyblend",
     "Nebel in Himmelsfarbe",
     "Wie stark ferner Nebel zum Himmel dahinter eingefärbt wird. 0, das ursprüngliche Aussehen, "
     "nimmt allein die Nebelfarbe der Zone; 1 gleicht ihn dem mittleren Himmelsband an.",
     nullptr, nullptr, nullptr},
    {"mistdensity",
     "Dunstdichte",
     "Wie dicht der beleuchtete Dunst ist, wenn Lichtstrahlen an sind (Details). 1 ist ein "
     "leichter Schleier; darüber zieht sich die Luft zu. Regen, Schnee, Morgengrauen und neblige "
     "Zonen verdichten ihn weiter.",
     nullptr, nullptr, nullptr},
    {"antialiasing",
     "Multisampling",
     "Glättet gezackte Kanten, indem jedes Pixel mehrfach abgetastet wird. 2x kostet wenig; 8x "
     "kostet bei hohen Auflösungen Speicher und Füllrate. Wirkt zusammen mit FXAA und "
     "FSR-Hochskalierung.",
     "Aus|2x|4x|8x", "MULTISAMPLE", nullptr},
    {"fxaa",
     "Kantenglättung (FXAA)",
     "Ein günstiger Glättungsdurchgang über das fertige Bild. Erfasst Kanten, die MSAA verfehlt, "
     "etwa Blätter und Gras, und macht feine Texturen etwas weicher. Am nützlichsten mit MSAA aus "
     "oder auf schwächerer Hardware.",
     nullptr, nullptr, nullptr},
    {"lensflare",
     "Blendenfleck",
     "Wie stark der Blendenfleck der Sonne ist, wenn sie im Bild steht. Nahe dem Horizont wird er "
     "bernsteinfarben, für Morgen- und Abendstimmung. 0 entfernt ihn. Nicht im ursprünglichen "
     "Client, der eine eigene Blendung hat.",
     nullptr, nullptr, nullptr},
    {"sharpstars",
     "Scharfe Sterne",
     "Die Sterne des Nachthimmels als klare Punkte zeichnen. Aus sind sie das Sternenmodell des "
     "ursprünglichen Clients, das bei hohen Auflösungen weich wird.",
     nullptr, nullptr, nullptr},
    {"sunshafts",
     "Sonnenstrahlen",
     "Lichtstrahlen, die von der Sonne durch Lücken zwischen Bäumen und Gebäuden fallen, wenn Ihr "
     "in ihre Richtung blickt. Günstig; über das fertige Bild gezeichnet. Nicht im ursprünglichen "
     "Client.",
     nullptr, nullptr, nullptr},
    {"shadows",
     "Dynamische Schatten",
     "Schatten, die die Sonne von allem in der Welt wirft, aus einer Schattenkarte. Aus ist das "
     "Standardaussehen des ursprünglichen Clients: die eingebrannten Schatten des Geländes.",
     nullptr, nullptr, nullptr},
    {"shadowdistance",
     "Schattenweite",
     "Wie weit entfernt Dinge noch Schatten werfen, in Metern. Die Schattenkarte deckt diese ganze "
     "Weite ab; eine kürzere Weite gibt daher auch schärfere Schatten in Eurer Nähe.",
     nullptr, nullptr, nullptr},
    {"shadowresolution",
     "Schattenauflösung",
     "Wie fein jede Schattenstufe gezeichnet wird. Höher gibt schärfere Schattenkanten mit weniger "
     "Treppenstufen. Jede Stufe verdoppelt die Seitenlänge jeder Karte und braucht viermal so viel "
     "Grafikspeicher. Mit einer Stufe ist 4x auf den meisten Rechnern 8192 mal 8192 (512 MB) und "
     "8x 16384 mal 16384 (2 GB). Zwei Stufen brauchen das Doppelte, drei das Vierfache, und mit "
     "mehr als einer ist 8x dasselbe wie 4x.",
     "Standard|2x|4x|8x", nullptr, nullptr},
    {"shadowcascades",
     "Schattenstufen",
     "In wie viele Karten die Schatten aufgeteilt werden. Die nächste deckt den Boden um Euch mit "
     "feinem Detail ab, die weiteren den Rest, sodass nahe Schatten viel schärfer sind. 1 ist eine "
     "einzige Karte über die ganze Schattenweite. Mehr Stufen brauchen mehr Grafikspeicher (siehe "
     "Schattenauflösung), und jede zeichnet die Schattenwerfer erneut.",
     "1|2|3", nullptr, nullptr},
    {"groundclutter",
     "Bodenobjektdichte",
     "Wie viele kleine Pflanzen, Grasbüschel und Steine über den Boden verstreut sind, in Prozent "
     "dessen, was die Zone vorsieht. 100 ist wie vorgesehen; darüber dichter, als der "
     "ursprüngliche Client zeichnete.",
     nullptr, "GROUND_DENSITY", nullptr},
    {"groundclutterdistance",
     "Bodenobjektradius",
     "Wie weit hinaus dieser Bewuchs gezeichnet wird, in Metern. Dahinter ist er schlicht nicht "
     "da; eine kurze Weite ist auf offenem Boden eine sichtbare Kante.",
     nullptr, "GROUND_RADIUS", nullptr},
    {"normalmapping",
     "Oberflächenrelief",
     "Stein, Ziegel und Holz von Gebäuden nach ihrer Oberflächentextur beleuchten, sodass sie das "
     "Licht wie das echte Material fangen statt wie flache Farbe. Nicht im ursprünglichen Client.",
     nullptr, nullptr, nullptr},
    {"normalmapstrength",
     "Reliefstärke",
     "Wie ausgeprägt dieses Relief wirkt. 1 ist wie die Texturen gemacht wurden; höher übertreibt "
     "es.",
     nullptr, nullptr, nullptr},
    {"particledensity",
     "Partikeldichte",
     "Wie viele Partikel ein Zauber, Feuer oder Wasserfall ausstößt, in Prozent dessen, was er "
     "verlangt. Niedriger dünnt alle Effekte zugleich aus.",
     nullptr, "PARTICLE_DENSITY", nullptr},
    {"weatherdetail",
     "Wetter",
     "Wie viel von Regen, Schnee und Sand gezeichnet wird: ein Zehntel, ein Drittel, zwei Drittel "
     "oder alles. Die weatherDensity des ursprünglichen Clients, die bei zwei Dritteln beginnt.",
     "Niedrig|Mäßig|Gut|Hoch", "WEATHER_DETAIL", nullptr},
    {"environmentdetail",
     "Umgebungsdetails",
     "Wie weit hinaus Doodads - Kisten, Büsche, Lampen, Zäune - weiter gezeichnet werden, in "
     "Prozent. Niedriger leert zuerst die mittlere Entfernung.",
     nullptr, "ENVIRONMENT_DETAIL", nullptr},
    {"texturefiltering",
     "Texturenfilter",
     "Wie scharf Texturen bleiben, wenn man sie flach ansieht - eine Straße voraus, ein Boden "
     "unter den Füßen. Kostet auf jeder modernen Karte wenig und gilt für ab jetzt geladene "
     "Texturen.",
     "Aus|2x|4x|8x|16x", "ANISOTROPIC", nullptr},
    {"lightshafts",
     "Lichtstrahlen und Dunst",
     "Dunst, durch den sich Licht bewegt: Die Sonne wirft Strahlen an Bäumen und Gebäuden vorbei, "
     "und Fackeln und Lava glühen darin. Höhere Stufen sind schärfer und kosten mehr; Niedrig "
     "passt zu schwächerer Hardware. Nicht im ursprünglichen Client, daher aus, bis gewählt.",
     "Aus|Niedrig|Mittel|Hoch", nullptr, nullptr},
    {"rtwater",
     "Wasser mit Raytracing",
     "Mit Raytracing: Wellen, die Spiegelung des Wassers und was darunter liegt, durch die Welt "
     "verfolgt - der Grund von den Wellen gebrochen, mit der Tiefe dunkler und blauer, Himmel und "
     "Ufer im Spiegel. Aus, oder ohne Raytracing, das Wasser des ursprünglichen Clients.",
     nullptr, nullptr, nullptr},
    {"rtwaveamp",
     "Wellenhöhe (%)",
     "Wie hoch die Wellen des Wassers mit Raytracing stehen: 0 ist ein stiller Spiegel, 200 das "
     "Doppelte des Standards.",
     nullptr, nullptr, nullptr},
    {"rtwavefreq",
     "Wellenfrequenz (%)",
     "Wie dicht die Wellen des Wassers mit Raytracing aufeinander folgen: niedriger sind lange "
     "Dünungen, höher kurzer Wellengang.",
     nullptr, nullptr, nullptr},
    {"specular",
     "Glanzlichter",
     "Die Option Glanzlichter des ursprünglichen Clients: Die Sonne glitzert auf Wasser und auf "
     "glänzenden Bodentexturen. Wirkt beim nächsten Spielstart, wie im ursprünglichen Client.",
     nullptr, nullptr, nullptr},
    {"raytracedlighting",
     "Raytracing (experimentell)",
     "SEHR EXPERIMENTELL: Bildfehler und hohe Kosten bei der Bildrate sind zu erwarten, besonders "
     "ohne Raytracing-Hardware. Verfolgt Strahlen durch die Welt für die Schatten der Sonne, die "
     "Verdunkelung in Ecken und Spalten und das von Boden und Wänden zurückgeworfene Licht. Nutzt "
     "das Raytracing der Grafikkarte, wo sie es hat, und sonst einen langsameren Rechenweg. "
     "Charaktere werfen ihre Schatten weiter aus der Schattenkarte.",
     "Aus|Sonnenschatten|Schatten und Verdeckung|Schatten, Verdeckung und indirektes Licht", nullptr, nullptr},
    {"rtfullres",
     "Volle Auflösung verfolgen",
     "Jedes Pixel verfolgen statt eines von vieren. Etwa das Vierfache der Kosten. Dünne Blätter, "
     "Gras, Pfähle und die Kanten der Dinge bekommen dann ihr eigenes verfolgtes Licht und ihren "
     "eigenen Schatten, statt des Aussehens ohne Raytracing, wo die halbe Auflösung sie verfehlte.",
     nullptr, nullptr, nullptr},
    {"rtsoftlamps",
     "Weicheres Lampenlicht",
     "Fackeln, Kerzen und Lampen leuchten etwas weiter um das herum, was sie anscheinen: "
     "weicheres, flacheres Lampenlicht, und die von einer niedrigen Kerze knapp abgewandten "
     "Flächen - die Kante einer Stufe, ein Bordstein - ein wenig erhellt statt dunkel.",
     nullptr, nullptr, nullptr},
    {"rtminbrightness",
     "Mindesthelligkeit (%)",
     "Wie viel ihrer Helligkeit ohne Raytracing eine Fläche behält, wo verfolgte Schatten, "
     "Verdeckung und die Schattierung eines Raumes sie zusammen ins Schwarze nähmen. Beleuchtete "
     "Orte bleiben unverändert; nur die fast schwarzen Ecken werden angehoben. 0 lässt sie ganz "
     "dunkel werden.",
     nullptr, nullptr, nullptr},
    {"rteyebrighten",
     "Aufhellung (%)",
     "Wie viel heller das Bild werden darf, wenn die Sicht dunkel ist, während sich das Auge "
     "anpasst: 30 ist bis zu 1,3-mal so hell, 100 doppelt. Eine grelle Sicht wird damit ein wenig "
     "gedämpft. 0 schaltet die Anpassung aus: das Bild wie im ursprünglichen Client.",
     nullptr, nullptr, nullptr},
    {"rteyeseconds",
     "Dauer (Sekunden)",
     "Wie lange sich das Auge an eine dunklere Sicht anpasst. An eine hellere fünfmal schneller "
     "(2,5 s hier sind dort 0,5 s). Länger macht aus dem Wechsel ein langsames Überblenden.",
     nullptr, nullptr, nullptr},
    {"rteyesensitivity",
     "Empfindlichkeit (%)",
     "Wie früh das Bild aufgehellt oder abgedunkelt wird: wie weit eine Sicht vom Tageslicht "
     "abweichen darf, bevor sich das Auge anpasst. 0 lässt einen weiten Bereich unberührt, wie es "
     "immer war; höher passt kleinere Unterschiede an; 100 zieht jede Sicht zur Mitte.",
     nullptr, nullptr, nullptr},
    {"rtsunrays",
     "Sonnenschatten",
     "Mehr Strahlen zur Sonne je Pixel: glattere Sonnenschatten und Kanten, weniger Flimmern in "
     "Bewegung. Jeder Strahl kostet die Sonnenschatten erneut.",
     nullptr, nullptr, nullptr},
    {"rtaorays",
     "Verdeckung",
     "Mehr Strahlen für die Verdunkelung in Ecken und Spalten (Umgebungsverdeckung): weniger "
     "Körnung darin. Jeder Strahl kostet sie erneut.",
     nullptr, nullptr, nullptr},
    {"rtpointrays",
     "Lampenschatten",
     "Mehr Strahlen zu den Fackeln, Lampen und leuchtenden Dingen nahe jedem Pixel: ruhigere "
     "Schatten, wo sich mehrere Lichter überlagern. Jeder Strahl kostet diese Schatten erneut.",
     nullptr, nullptr, nullptr},
    {"rtbouncerays",
     "Indirektes Licht",
     "Mehr Strahlen für jede Richtung der Lichtsonden, die das indirekte Licht tragen: weniger "
     "Flackern darin, wenn sich die Sicht bewegt. Jeder Strahl kostet die Verfolgung der Sonden "
     "erneut.",
     nullptr, nullptr, nullptr},
    {"dbgview",
     "Anstelle des Bildes zeigen",
     "Ersetzt das Bild durch einen Teil der Beleuchtung mit Raytracing. Die verfolgten Ansichten "
     "gehen in jedem Raytracing-Modus; die Sondenansichten brauchen Schatten, Verdeckung und "
     "indirektes Licht; was das eingebrannte Licht eines Gebäudes färbt - seine Räume, deren "
     "Einrichtung, wer auf ihren Böden steht - bekommt kein Sondenlicht und zeigt sich wie es ist. "
     "Nichts bei ausgeschaltetem Raytracing. Die Verlaufslänge ist grün, wo eine Fläche über viele "
     "Bilder verfolgt wurde, und rot, wo sie neu beginnt; die nächste Ansicht ist grün, wo ein "
     "Pixel seine eigene Fläche fand, und rot, wo es wie ohne Raytracing beleuchtet ist. Die "
     "eingebrannte Schattierung ist weiß, wo die gemalte Farbe eines Gebäudes wie gemalt "
     "erscheint, cyan, wo die Schatten seiner Lampen sie abdunkeln, rot, wo es das Licht seines "
     "Raumes tut, und dunkelblau, wo nichts gemalt ist. Das hinzugefügte Punktlicht ist, was die "
     "Lampen jeder Fläche hinzufügen; der Faktor der eingebrannten Farbe, womit ihr gemaltes Licht "
     "skaliert wird (grau; weiß wie gemalt, rot wo nichts gemalt ist). Die Art jedes Lichts "
     "zeichnet das Licht jeder Lampe in der Farbe dessen, was es gab - Feuer, Lampe, Leuchten, "
     "Gebäude, Einheit, Modell, Partikel - mit einem Punkt an jedem Licht und einer Legende der "
     "Farben. Die Ansichten des Wassers mit Raytracing zeigen seine Spiegelung allein, was "
     "hindurchkommt allein, und die Spiegelung wie verfolgt - nicht die eigene Farbe des Bildes, "
     "wo sie auf dem Schirm ist. WOWEE_GI_DEBUG, wenn gesetzt, hat Vorrang.",
     "Aus|Verhältnis des Sondenlichts (grau = offenes Feld)|Sondeneinstrahlung|Umgebungslicht mit "
     "Sonden|Sondenstufen|Verfolgte Sonnensicht|Licht der Punktlichter|Verfolgte Verdeckung|Länge "
     "des verfolgten Verlaufs|Wie jedes Pixel seine Fläche fand|Eingebrannte "
     "Schattierung|Tiefenkanten|Hinzugefügtes Punktlicht|Faktor der eingebrannten Farbe|Art jedes "
     "Lichts|Wasser mit Raytracing: Spiegelung|Wasser mit Raytracing: was hindurchkommt|Wasser mit "
     "Raytracing: Spiegelung, nur verfolgt", nullptr, nullptr},
    {"dbgambient",
     "Umgebungslicht (%)",
     "Das Umgebungslicht der Zone, die Grundlage, von der jede Fläche vor Sonne und Lampen "
     "ausgeht. 100 ist das der Zone.",
     nullptr, nullptr, nullptr},
    {"dbgsun",
     "Sonnenlicht (%)",
     "Das direkte Licht von Sonne oder Mond. 100 ist das der Zone.",
     nullptr, nullptr, nullptr},
    {"dbgbakedlight",
     "Gemaltes Licht (%)",
     "Das in die Welt gemalte Licht: die Vertexfarben eines Gebäudeinneren, die Farben seiner "
     "Doodads und die Bodenfarbe, die Einheiten darin annehmen. 100 ist wie gemalt.",
     nullptr, nullptr, nullptr},
    {"dbgpointlights",
     "Lampen, Raytracing (%)",
     "Das Licht, das Fackeln, Feuer, Lampen und Laternen unter der Beleuchtung mit Raytracing "
     "hinzufügen. 100 ist ihr eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgbounce",
     "Indirektes Licht (%)",
     "Wie weit das indirekte Licht der Sonden schattige Orte abdunkelt und helle anhebt (Schatten, "
     "Verdeckung und indirektes Licht). 0 lässt das flache Umgebungslicht.",
     nullptr, nullptr, nullptr},
    {"dbginteriorbase",
     "Innen ohne Lampen (%)",
     "Wie viel vom gemalten Licht eines Raumes sich zeigt, wo kein Lampenlicht hinreicht: Wände, "
     "Böden, die Dinge und Leute darin. Wo die Lampen voll scheinen, zeigt sich alles. 100 ist wie "
     "gemalt.",
     nullptr, nullptr, nullptr},
    {"dbginteriorfullat",
     "Volle Farbe ab (%)",
     "Wie hell das verfolgte Licht der Lampen sein muss, damit sich das gemalte Licht eines Raumes "
     "ganz zeigt. Niedriger erhellt mehr vom Raum.",
     nullptr, nullptr, nullptr},
    {"rtnormalkeep",
     "Eigene Normale bis (°)",
     "Lampen beleuchten eine Fläche nach ihrer eigenen (geglätteten, reliefierten) Normale, "
     "solange sie höchstens so weit von der echten Fläche abweicht: runde Säulen, Bögen und "
     "Charaktere bleiben glatt. Darüber übernimmt die Fläche.",
     nullptr, nullptr, nullptr},
    {"rtnormalface",
     "Flächennormale ab (°)",
     "Ab diesem Winkel von ihrer Fläche wird eine Fläche allein nach der Fläche beleuchtet, sodass "
     "ein Boden, dessen Normalen mit den Wänden geglättet wurden, nicht fleckig dunkel wird. "
     "Dazwischen eine weiche Mischung; nie weniger als der erste Wert.",
     nullptr, nullptr, nullptr},
    {"dbgtimeoverride",
     "Diese Tageszeit verwenden",
     "Die Welt zur unten gewählten Zeit beleuchten statt zu der des Servers. Nur was gezeichnet "
     "wird; die Uhr des Servers bleibt unberührt.",
     nullptr, nullptr, nullptr},
    {"dbgtime",
     "Tageszeit (Stunden)",
     "Die Tageszeit, zu der die Welt beleuchtet wird, solange der Schalter oben an ist: 0 und 24 "
     "sind Mitternacht, 12 ist Mittag.",
     nullptr, nullptr, nullptr},
    {"dbgmodellightintensity",
     "Modelle: Stärke (%)",
     "Die Lichter, die Modelldateien mitbringen: Kohlenbecken, Kristalle, Zaubereffekte an "
     "Objekten. Raytracing. 100 ist sein eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgmodellightreach",
     "Modelle: Weite (%)",
     "Wie weit sein Licht reicht: wo der Abfall beginnt und endet. Raytracing. 100 ist sein "
     "eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgfirelightintensity",
     "Feuer: Stärke (%)",
     "Feuer, Kohlenbecken und Fackeln, deren Dateien kein Licht mitbringen: ein warmes, "
     "flackerndes Licht an ihrer Flamme. Raytracing. 100 ist sein eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgfirelightreach",
     "Feuer: Weite (%)",
     "Wie weit sein Licht reicht: wo der Abfall beginnt und endet. Raytracing. 100 ist sein "
     "eigenes.",
     nullptr, nullptr, nullptr},
    {"dbglamplightintensity",
     "Lampen: Stärke (%)",
     "Straßenlampen, Laternen und Wandleuchten, deren Dateien kein Licht mitbringen, zu jeder "
     "Stunde beleuchtet. Raytracing. 100 ist sein eigenes.",
     nullptr, nullptr, nullptr},
    {"dbglamplightreach",
     "Lampen: Weite (%)",
     "Wie weit sein Licht reicht: wo der Abfall beginnt und endet. Raytracing. 100 ist sein "
     "eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgbuildinglightintensity",
     "Gebäude: Stärke (%)",
     "Die eigenen Lampen eines Gebäudes (WMO-Lichter). Seine Wände und Böden tragen ihr Licht "
     "schon; der Rest bekommt es aus der Verfolgung. Raytracing. 100 ist sein eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgbuildinglightreach",
     "Gebäude: Weite (%)",
     "Wie weit sein Licht reicht: wo der Abfall beginnt und endet. Raytracing. 100 ist sein "
     "eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgunitlightintensity",
     "Einheiten: Stärke (%)",
     "Die Lichter von Zaubereffekten und Modellen an Charakteren und Kreaturen. Raytracing. 100 "
     "ist sein eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgunitlightreach",
     "Einheiten: Weite (%)",
     "Wie weit sein Licht reicht: wo der Abfall beginnt und endet. Raytracing. 100 ist sein "
     "eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgmaxlight",
     "Höchstes Licht (%)",
     "Das meiste Licht, das die verfolgten Lichter zusammen einem Ort hinzufügen dürfen: jede "
     "Lampe, jedes Feuer, Leuchten und Lava, die ihn erreichen, summiert und darauf begrenzt, ihre "
     "Farbe erhalten. 100 ist das Licht eines vollen Weiß über dem dort Gemalten, der Standard. "
     "1000 ist keine Grenze.",
     nullptr, nullptr, nullptr},
    {"dbglightcountlimit",
     "Höchstzahl der Lichter",
     "Wie viele Punktlichter die Verfolgung je Bild höchstens behält. Darüber behält eine volle "
     "Ansicht die wichtigsten nahe der Kamera, und ein Licht kann wegfallen, wenn sich die Kamera "
     "bewegt. Mehr kostet Verfolgungszeit. 512 ist der Standard.",
     nullptr, nullptr, nullptr},
    {"dbgemissivelightintensity",
     "Leuchten: Stärke (%)",
     "Was an Modellen und Gebäuden leuchtet: Lampenglas, Leuchtkarten, Lava, Kristalle, leuchtende "
     "Teile. Raytracing. 100 ist sein eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgemissivelightreach",
     "Leuchten: Weite (%)",
     "Wie weit sein Licht reicht: wo der Abfall beginnt und endet. Raytracing. 100 ist sein "
     "eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgparticlelightintensity",
     "Partikel: Stärke (%)",
     "Leuchtende Partikeleffekte - Zauber, Flammen, Kerzen - ein Licht je Emitter in der Mitte "
     "seiner Partikel. Raytracing. 100 ist sein eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgparticlelightreach",
     "Partikel: Weite (%)",
     "Wie weit sein Licht reicht: wo der Abfall beginnt und endet. Raytracing. 100 ist sein "
     "eigenes.",
     nullptr, nullptr, nullptr},
    {"dbgshadowdarkness",
     "Schattendunkelheit (%)",
     "Wie viel des Lichts ein Schatten nimmt, wo das Raytracing ein Ergebnis hat. Ohne es gelten "
     "die 30 % des Clients.",
     nullptr, nullptr, nullptr},
    {"dbgtracedsunshadow",
     "Sonne, verfolgt (%)",
     "Wie viel der verfolgten Sonnenschatten sich zeigt. Einheiten in Eurer Nähe werfen ihre "
     "stattdessen über die Schattenkarte.",
     nullptr, nullptr, nullptr},
    {"dbgpointshadow",
     "Lampenschatten (%)",
     "Wie viel der Schatten, die Fackeln und Lampen werfen, sich zeigt.",
     nullptr, nullptr, nullptr},
    {"dbgbakedlampshadow",
     "Auf gemaltem Licht (%)",
     "Wie viel vom gemalten Licht eines Innenraums der Schatten einer Lampe nimmt, wo diese Lampe "
     "ihn allein beleuchtet.",
     nullptr, nullptr, nullptr},
    {"dbgocclusion",
     "Verdeckung (%)",
     "Wie viel der verfolgten Verdunkelung in Ecken und Spalten sich zeigt.",
     nullptr, nullptr, nullptr},
    {"dbgmapsunshadow",
     "Sonne, Karte (%)",
     "Wie viel der Sonnenschatten der Schattenkarte sich zeigt, mit Raytracing an oder aus. Sie "
     "tragen die Schatten der Einheiten und ihre eigenen.",
     nullptr, nullptr, nullptr},
    {"dbgterrainshadow",
     "Geländeschatten (%)",
     "Die Schatten, die unter Bäumen und Gebäuden in den Boden gemalt sind (und auf das Gras "
     "darauf). 100 ist, wie der Client sie zeichnet; sie zeigen sich nur mit dynamischen Schatten "
     "aus, da die Schattenkarte ihren Platz einnimmt.",
     nullptr, nullptr, nullptr},
    {"dbgblobshadow",
     "Runde Schatten (%)",
     "Die runden Schatten unter Einheiten bei ausgeschalteten Schatten und Raytracing.",
     nullptr, nullptr, nullptr},
    {"upscaling",
     "Hochskalierung",
     "Die Welt kleiner zeichnen und dann auf den Bildschirm hochskalieren, für mehr Bilder je "
     "Sekunde. FSR 1 ist eine günstige, schärfende Hochskalierung. FSR 3 baut Details aus früheren "
     "Bildern wieder auf: schärfer und ruhiger, und es glättet auch Kanten. Beide wirken mit MSAA "
     "und FXAA.",
     "Aus|FSR 1 (räumlich, günstig)|FSR 3 (zeitlich, schärfer)", nullptr, nullptr},
    {"fsrquality",
     "Renderauflösung",
     "Wie groß die Welt vor dem Hochskalieren gezeichnet wird, als Anteil des Bildschirms. Kleiner "
     "ist schneller und weicher. Nativ behält die volle Größe und nutzt FSR 3 allein zur "
     "Kantenglättung.",
     "Ultra-Qualität (77 %)|Qualität (67 %)|Ausgewogen (59 %)|Nativ (100 %)", nullptr, nullptr},
    {"fsrsharpness",
     "Nachschärfen",
     "Schärfung nach dem Hochskalieren. Erhöhen, wenn das Bild weich wirkt; zu weit, und Kanten "
     "bekommen helle Säume.",
     nullptr, nullptr, nullptr},
    {"grassenabled",
     "Gras (experimentell)",
     "Gras aus den eigenen Bodeneffektdaten des Geländes wachsen lassen, das sich im Wind biegt "
     "und teilt, wenn Ihr hindurchgeht. Experimentell: Es kostet Zeit auf dem Hauptthread, wenn "
     "Ihr Euch bewegt, und hat bekannte Fehler.",
     nullptr, nullptr, nullptr},
    {"grassdensity",
     "Grasdichte",
     "Wie viel Gras wächst, in Prozent dessen, was das Gelände vorsieht. 100 ist wie vorgesehen.",
     nullptr, nullptr, nullptr},
    {"grassheight",
     "Grashöhe",
     "Wie hoch die Halme wachsen, in Prozent. Höheres Gras zeigt den Wind deutlicher, der darüber "
     "streicht.",
     nullptr, nullptr, nullptr},
    {"grassdistance",
     "Grasweite",
     "Wie weit hinaus Gras gezeichnet wird, in Metern. Über 45 wird das Feld mit der Entfernung "
     "dünner, und jeder Halm wird eingeblendet, wenn Ihr darauf zureitet. Weiter kostet bei jeder "
     "Bewegung mehr.",
     nullptr, nullptr, nullptr},
    {"fsrjittersign",
     "Jitter-Vorzeichen",
     "In welche Richtung der Subpixel-Versatz von FSR 3 angewandt wird. 0,38 ist der Wert, der "
     "derzeit richtig aussieht; der Rest des Bereichs dient der Fehlersuche.",
     nullptr, nullptr, nullptr},
    {"fullscreen",
     "Vollbild",
     "Den Bildschirm füllen statt in einem Fenster zu laufen. Wirkt, wenn das Fenster das nächste "
     "Mal neu aufgebaut wird, oder beim nächsten Start.",
     nullptr, nullptr, nullptr},
    {"vsync",
     "Vertikal-Synch",
     "Jedes Bild im Takt der Anzeige zeigen. Beseitigt Bildrisse und begrenzt die Bildrate auf die "
     "Bildwiederholrate der Anzeige.",
     nullptr, "VERTICAL_SYNC", nullptr},
    {"framecap",
     "Bildratenbegrenzung",
     "Die meisten Bilder, die je Sekunde gezeichnet werden - die fps-Grenze. Unbegrenzt lässt die "
     "Hardware voll laufen, was auf einem Laptop Wärme und Lüfterlärm für Bilder bedeutet, die "
     "niemand sieht. Die vertikale Synchronisation begrenzt schon auf die Rate der Anzeige; dies "
     "begrenzt darunter.",
     "Unbegrenzt|30|60|90|120|144|240", nullptr, nullptr},
    {"mapwindow",
     "Karte im eigenen Fenster",
     "Die Weltkarte in einem eigenen Fenster, für einen zweiten Bildschirm. Sie folgt Eurer Zone "
     "und lässt sich durchblättern; in Ruhe gelassen, kehrt sie zu Euch zurück. Zieht sie, wohin "
     "Ihr wollt - dort öffnet sie sich wieder.",
     nullptr, nullptr, nullptr},
    {"brightness",
     "Helligkeit",
     "Wie hell das Bild ist. 50 lässt es, wie die Zone beleuchtet ist; darunter dunkler, darüber "
     "werden die Schatten angehoben. Das eigene Fenster des Spiels nannte dies Gamma und erreichte "
     "denselben Wert.",
     nullptr, nullptr, nullptr},
    {"screenglow",
     "Vollbild-Leuchten",
     "Ein weiches Leuchten über den hellen Teilen des Bildes, so stark, wie das Licht der Zone "
     "verlangt. Das ffxGlow des ursprünglichen Clients, standardmäßig an.",
     nullptr, nullptr, nullptr},
    {"texturemagnification",
     "Nahfilterung",
     "Wie eine Textur aussieht, wenn Ihr nah genug seid, dass ihre Pixel sichtbar werden. Bilinear "
     "ist die des ursprünglichen Spiels; Weich rundet die kantigen Stufen ab. Kostet auf nahen "
     "Flächen etwas. Die Stufe Niedrig und Telefone zeichnen immer Bilinear.",
     "Bilinear|Weich", nullptr, nullptr},
    {"fov",
     "Sichtfeld",
     "Wie hoch das Bild ist, das die Kamera erfasst, in Grad. 54 zeigt der ursprüngliche Client; "
     "breiter fasst mehr und streckt die Ränder.",
     nullptr, nullptr, nullptr},
    {"camerashake",
     "Kamerawackeln",
     "Wie stark sich die Sicht von selbst bewegt: Zaubereffekte, Donner und das Schwanken beim "
     "Betrunkensein. 0 hält sie an. Das schiefe Gehen beim Betrunkensein bleibt - das geschieht "
     "Eurem Charakter, nicht dem Bild.",
     nullptr, nullptr, nullptr},
    {"cameramaxdistance",
     "Größter Kameraabstand",
     "Wie weit die Kamera zurückgezogen werden kann, in Metern. 22 gibt der ursprüngliche Client; "
     "weiter weg zeigt mehr vom Kampf und weniger von Eurem Charakter.",
     nullptr, nullptr, nullptr},
    {"pivotheight",
     "Drehpunkthöhe",
     "Der Punkt, um den sich die Kamera dreht, als Höhe über Euren Füßen in Metern. Niedriger "
     "fühlt sich näher am Charakter an; höher schaut über ihn hinweg.",
     nullptr, nullptr, nullptr},
    {"idleorbit",
     "Kreisen im Stillstand",
     "Die Kamera langsam um Euch kreisen lassen, während Ihr stillsteht.",
     nullptr, nullptr, nullptr},
    {"mousespeed",
     "Mausempfindlichkeit",
     "Wie weit sich die Sicht bei einer Mausbewegung dreht. Das eigene Fenster des Spiels bot dies "
     "zweimal an, als Empfindlichkeit und als Blicktempo, und beide schrieben diesen einen Wert.",
     nullptr, "MOUSE_SENSITIVITY", nullptr},
    {"invertmouse",
     "Maus umkehren",
     "Maus nach vorn schieben, um nach oben zu sehen, wie in einem Flugsimulator.",
     nullptr, "INVERT_MOUSE", nullptr},
    {"gamepad",
     "Controller verwenden",
     "Einen angeschlossenen Controller bewegen, schauen, anvisieren und zaubern lassen. Der linke "
     "Stick geht, der rechte schaut, die Trigger zoomen, und die Tasten und das Steuerkreuz sind "
     "die ersten sechs Aktionsplätze - linke Schultertaste halten für die anderen sechs.",
     nullptr, nullptr, nullptr},
    {"gamepadlookspeed",
     "Controller-Blicktempo",
     "Wie schnell der rechte Stick die Sicht dreht, in Grad je Sekunde bei vollem Ausschlag. "
     "Getrennt von der Maus: Es sind verschiedene Instrumente, und das eine zu verlangsamen sagt "
     "nichts über das andere.",
     nullptr, nullptr, nullptr},
    {"gamepadinvertlook",
     "Controllerblick umkehren",
     "Rechten Stick nach vorn schieben, um nach unten zu sehen.",
     nullptr, nullptr, nullptr},
    {"gamepaddeadzone",
     "Controller-Totzone",
     "Wie viel jedes Sticks als Ruhestellung zählt. Erhöhen für einen abgenutzten Stick, der "
     "driftet, wenn ihn niemand berührt; senken für feinere Steuerung.",
     nullptr, nullptr, nullptr},
    {"uiopacity",
     "Fensterdeckkraft",
     "Wie undurchsichtig die eigenen Fenster dieses Clients sind: Einstellungen, Anzeigen und "
     "dergleichen. 100 ist deckend.",
     nullptr, nullptr, nullptr},
    {"windowuiscale",
     "Fenstergröße",
     "Größe von Text und Bedienelementen in den eigenen Fenstern dieses Clients. Die eigene "
     "Skalierung der Spieloberfläche steht im Grafikfenster des Spiels.",
     nullptr, nullptr, nullptr},
    {"scrollspeed",
     "Scrolltempo",
     "Wie weit das Mausrad oder ein Touchpad Text in Chat, Questlog und anderen Fenstern scrollt. "
     "1 ist eine Zeile je Raste.",
     nullptr, nullptr, nullptr},
    {"latencymeter",
     "Latenzanzeige",
     "Euren Ping - die Hin- und Rücklaufzeit zum Server - oben am Bildschirm zeigen.",
     nullptr, nullptr, nullptr},
    {"showfps",
     "Bildrate anzeigen",
     "Oben am Bildschirm zeigen, wie viele Bilder je Sekunde das Spiel zeichnet.",
     nullptr, nullptr, nullptr},
    {"micromenu",
     "Mikromenü-Schaltflächen",
     "Die Reihe der Kurzwahlschaltflächen zu Charakterfenster, Zauberbuch, Talenten und dem Rest.",
     nullptr, nullptr, nullptr},
    {"bagscale",
     "Taschengröße",
     "Größe der Taschenfenster.",
     nullptr, nullptr, nullptr},
    {"separatebags",
     "Getrennte Taschenfenster",
     "Jede Tasche in einem eigenen Fenster öffnen statt alle in einem.",
     nullptr, nullptr, nullptr},
    {"showkeyring",
     "Schlüsselbund zeigen",
     "Die Schaltfläche des Schlüsselbunds neben den Taschen zeigen.",
     nullptr, nullptr, nullptr},
    {"freebagslots",
     "Freien Inventarplatz zeigen",
     "Die leeren Plätze aller Taschen auf der Rucksackschaltfläche zählen.",
     nullptr, "DISPLAY_FREE_BAG_SLOTS", "OPTION_TOOLTIP_DISPLAY_FREE_BAG_SLOTS"},
    {"enhancedtooltips",
     "Erweiterte Tooltips",
     "Ausführlichere Tooltips zu Fähigkeiten: was eine tut, in Worten, nicht nur ihre Zahlen.",
     nullptr, "USE_UBERTOOLTIPS", "OPTION_TOOLTIP_USE_UBERTOOLTIPS"},
    {"beginnertips",
     "Einsteigertooltips",
     "Die Grundlagen in Tooltips erklären - wofür jeder Teil der Oberfläche da ist. Lohnt sich "
     "auszuschalten, sobald Ihr es wisst.",
     nullptr, "SHOW_NEWBIE_TIPS_TEXT", "OPTION_TOOLTIP_SHOW_NEWBIE_TIPS"},
    {"colorblindmode",
     "Farbenblind-Modus",
     "Die Reaktion einer Einheit und die Qualität eines Gegenstands in Tooltips in Worten nennen, "
     "nicht nur durch Farbe, und anderswo Hinweise ergänzen.",
     nullptr, "USE_COLORBLIND_MODE", "OPTION_TOOLTIP_USE_COLORBLIND_MODE"},
    {"showitemlevel",
     "Gegenstandsstufe zeigen",
     "Die Stufe eines Gegenstands in seinem Tooltip zeigen.",
     nullptr, nullptr, "OPTION_TOOLTIP_SHOW_ITEM_LEVEL"},
    {"equipmentmanager",
     "Ausrüstungsmanager benutzen",
     "Ausrüstungssets im Charakterfenster speichern und mit einem Klick wechseln.",
     nullptr, "USE_EQUIPMENT_MANAGER", nullptr},
    {"previewtalents",
     "Talentwechselvorschau",
     "Talentpunkte im Talentfenster vorab verteilen und dann alle auf einmal bestätigen.",
     nullptr, "PREVIEW_TALENT_CHANGES", nullptr},
    {"minimapclock",
     "Uhr",
     "Die Tageszeit unter der Minikarte zeigen.",
     nullptr, nullptr, nullptr},
    {"minimapsquare",
     "Eckige Minikarte",
     "Die Minikarte als Quadrat statt als Kreis zeichnen.",
     nullptr, nullptr, nullptr},
    {"minimapnpcdots",
     "Punkte für nahe Kreaturen",
     "Kreaturen in Eurer Nähe als Punkte auf der Minikarte markieren.",
     nullptr, nullptr, nullptr},
    {"minimapcoords",
     "Koordinaten",
     "Eure Kartenkoordinaten unter der Minikarte zeigen.",
     nullptr, nullptr, nullptr},
    {"minimaprotate",
     "Mit der Kamera drehen",
     "Die Minikarte so drehen, dass Eure Blickrichtung oben ist statt Norden. Das eigene Menü der "
     "Minikarte hat dies auch.",
     nullptr, nullptr, nullptr},
    {"actionbarscale",
     "Aktionsleisten-Größe",
     "Größe jeder Schaltfläche der Aktionsleisten.",
     nullptr, nullptr, nullptr},
    {"buffbarscale",
     "Stärkungsleisten-Größe",
     "Größe der Symbole für Stärkungs- und Schwächungszauber, zusätzlich zur automatischen "
     "Skalierung für Eure Anzeige.",
     nullptr, nullptr, nullptr},
    {"showbar2",
     "Leiste unten links",
     "Eine zweite Leiste über der Hauptleiste. Trägt die Aktionsseite 6 des Spiels, wo der "
     "ursprüngliche Client diese Leiste hat.",
     nullptr, nullptr, nullptr},
    {"bar2offsetx",
     "Untere Leiste: quer",
     "Diese Leiste von ihrem üblichen Platz nach links oder rechts verschieben, in Pixeln.",
     nullptr, nullptr, nullptr},
    {"bar2offsety",
     "Untere Leiste: hoch",
     "Diese Leiste von ihrem üblichen Platz nach oben oder unten verschieben, in Pixeln.",
     nullptr, nullptr, nullptr},
    {"showrightbar",
     "Rechte Seitenleiste",
     "Eine senkrechte Leiste am rechten Bildschirmrand. Trägt die Aktionsseite 3.",
     nullptr, nullptr, nullptr},
    {"rightbaroffsety",
     "Rechte Leiste: hoch",
     "Sie von der Mitte des Bildschirms nach oben oder unten verschieben, in Pixeln.",
     nullptr, nullptr, nullptr},
    {"showleftbar",
     "Linke Seitenleiste",
     "Eine senkrechte Leiste am linken Bildschirmrand. Trägt die Aktionsseite 4.",
     nullptr, nullptr, nullptr},
    {"leftbaroffsety",
     "Linke Leiste: hoch",
     "Sie von der Mitte des Bildschirms nach oben oder unten verschieben, in Pixeln.",
     nullptr, nullptr, nullptr},
    {"dpsmeter",
     "Schadensanzeige",
     "Euren Schaden und Eure Heilung je Sekunde im Kampf über der Aktionsleiste zeigen.",
     nullptr, nullptr, nullptr},
    {"cooldowntracker",
     "Abklingzeitanzeige",
     "Eure längeren Abklingzeiten neben der Aktionsleiste herunterzählen.",
     nullptr, nullptr, nullptr},
    {"raretracker",
     "Anzeige seltener Gegner",
     "Seltene Kreaturen in Eurer Nähe auf der Minikarte und der Weltkarte markieren.",
     nullptr, nullptr, nullptr},
    {"chesttracker",
     "Truhenanzeige",
     "Schatztruhen in Eurer Nähe auf der Minikarte markieren.",
     nullptr, nullptr, nullptr},
    {"damageflash",
     "Schadensblitz",
     "Einen roten Rand um den Bildschirm aufblitzen lassen, wenn Ihr getroffen werdet.",
     nullptr, nullptr, nullptr},
    {"lowhealthvignette",
     "Gesundheitswarnung",
     "Einen roten Rand um den Bildschirm pulsieren lassen, solange Eure Gesundheit unter einem "
     "Fünftel ist.",
     nullptr, nullptr, nullptr},
    {"screenedgeflash",
     "Blinken des Bildschirmrands",
     "Das eigene rote Blinken des Spiels am Bildschirmrand, solange Eure Gesundheit niedrig ist - "
     "neben der Warnung oben, die die dieses Clients ist.",
     nullptr, "SHOW_FULLSCREEN_STATUS_TEXT", "OPTION_TOOLTIP_SHOW_FULLSCREEN_STATUS"},
    {"buffdurations",
     "Stärkungszauberdauer",
     "Die verbleibende Zeit unter jedem Symbol eines Stärkungszaubers herunterzählen.",
     nullptr, "SHOW_BUFF_DURATION_TEXT", "OPTION_TOOLTIP_SHOW_BUFF_DURATION"},
    {"dispellabledebuffs",
     "Bannbare Zauber",
     "Die Schwächungszauber auf Eurem Ziel umrahmen, die einer Eurer Zauber bannen kann.",
     nullptr, "SHOW_DISPELLABLE_DEBUFFS_TEXT", "OPTION_TOOLTIP_SHOW_DISPELLABLE_DEBUFFS"},
    {"castablebuffs",
     "Wirkbare Zauber",
     "Im Zielfenster nur die Stärkungszauber zeigen, die Ihr selbst wirken könntet.",
     nullptr, "SHOW_CASTABLE_BUFFS_TEXT", "OPTION_TOOLTIP_SHOW_CASTABLE_BUFFS"},
    {"castabledebuffs",
     "Wirkbare Schwächungszauber",
     "Im Zielfenster nur die Schwächungszauber zeigen, die von Euch sind.",
     nullptr, "SHOW_CASTABLE_DEBUFFS_TEXT", "OPTION_TOOLTIP_SHOW_CASTABLE_DEBUFFS"},
    {"assistattack",
     "Angriff bei Unterstützen",
     "Angreifen, wenn /assist Euch ein feindliches Ziel gibt.",
     nullptr, "ASSIST_ATTACK", "OPTION_TOOLTIP_ASSIST_ATTACK"},
    {"stopautoattack",
     "Autom. Angriff stoppen",
     "Ein neues Ziel beendet Euren automatischen Angriff, statt ihn mitzunehmen.",
     nullptr, "STOP_AUTO_ATTACK", "OPTION_TOOLTIP_STOP_AUTO_ATTACK"},
    {"secureabilitytoggle",
     "Doppeltes Angreifen sperren",
     "Ein zweiter Druck auf Angriff innerhalb einer halben Sekunde wird ignoriert, sodass ein "
     "Doppeltippen den automatischen Angriff nicht gleich wieder ausschaltet.",
     nullptr, nullptr, nullptr},
    {"autofacetarget",
     "Zum Ziel drehen (Debug)",
     "Euren Charakter zum Ziel drehen, wenn Ihr es angreift oder darauf zaubert. Aus, wie im "
     "ursprünglichen Client, muss ein Ziel hinter Euch erst angesehen werden. Eine Hilfe zur "
     "Fehlersuche.",
     nullptr, nullptr, nullptr},
    {"autoselfcast",
     "Autom. Selbstzauber",
     "Ein hilfreicher Zauber ohne freundliches Ziel trifft Euch, statt auf eines zu warten.",
     nullptr, "AUTO_SELF_CAST_TEXT", "OPTION_TOOLTIP_AUTO_SELF_CAST"},
    {"selfcastkey",
     "Taste für Selbstzauber",
     "Diese beim Zaubern halten, um den Zauber auf Euch selbst zu wirken, was auch immer anvisiert "
     "ist.",
     "Keine|Alt|Strg|Umschalt", nullptr, nullptr},
    {"focuscastkey",
     "Taste für Fokuszauber",
     "Diese beim Zaubern halten, um den Zauber stattdessen auf Euer Fokusziel zu wirken.",
     "Keine|Alt|Strg|Umschalt", "FOCUS_CAST_KEY_TEXT", nullptr},
    {"targetoftarget",
     "Ziel des Ziels",
     "Ein kleines Fenster neben dem des Ziels, das zeigt, was es anvisiert.",
     nullptr, "SHOW_TARGET_OF_TARGET_TEXT", "OPTION_TOOLTIP_SHOW_TARGET_OF_TARGET"},
    {"targetoftargetwhen",
     "Anzeigen",
     "Wann dieses Fenster den Platz wert ist.",
     "Immer|Im Schlachtzug|In der Gruppe|Allein|In Schlachtzug oder Gruppe", nullptr, nullptr},
    {"targetcastbar",
     "Zauberleiste am Zielfenster",
     "Unter dem Fenster Eures Ziels zeigen, was es wirkt.",
     nullptr, nullptr, "OPTION_TOOLTIP_SHOW_TARGET_CASTBAR"},
    {"myname",
     "Mein Name",
     "Euer eigener Name über Eurem Kopf.",
     nullptr, "UNIT_NAME_OWN", "OPTION_TOOLTIP_UNIT_NAME_OWN"},
    {"npcnames",
     "NSC-Namen",
     "Namen über Kreaturen, mit denen Ihr sprechen könnt, und über feindlichen.",
     nullptr, "UNIT_NAME_NPC", "OPTION_TOOLTIP_UNIT_NAME_NPC"},
    {"critternames",
     "Tiere und Haustiere",
     "Namen über Hasen, Eichhörnchen und den Haustieren, die Euch nur folgen.",
     nullptr, "UNIT_NAME_NONCOMBAT_CREATURE", "OPTION_TOOLTIP_UNIT_NAME_NONCOMBAT_CREATURE"},
    {"guildnames",
     "Gildennamen",
     "Die Gilden der Spieler unter ihren Namen.",
     nullptr, "UNIT_NAME_GUILD", "OPTION_TOOLTIP_UNIT_NAME_GUILD"},
    {"playertitles",
     "Titel",
     "Die Titel der Spieler mit ihren Namen.",
     nullptr, "UNIT_NAME_PLAYER_TITLE", "OPTION_TOOLTIP_UNIT_NAME_PLAYER_TITLE"},
    {"friendlynames",
     "Freundliche Spieler",
     "Namen über Spielern Eurer Seite.",
     nullptr, "UNIT_NAME_FRIENDLY", "OPTION_TOOLTIP_UNIT_NAME_FRIENDLY"},
    {"friendlypetnames",
     "Freundliche Begleiter",
     "Namen über ihren Begleitern.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAME_FRIENDLY_PETS"},
    {"friendlyguardiannames",
     "Freundliche Wächter",
     "Namen über ihren Wächtern - herbeigerufenen Helfern, die kämpfen.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAME_FRIENDLY_GUARDIANS"},
    {"friendlytotemnames",
     "Freundliche Totems",
     "Namen über ihren Totems.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAME_FRIENDLY_TOTEMS"},
    {"enemynames",
     "Feindliche Spieler",
     "Namen über Spielern der anderen Fraktion.",
     nullptr, "UNIT_NAME_ENEMY", "OPTION_TOOLTIP_UNIT_NAME_ENEMY"},
    {"enemypetnames",
     "Feindliche Begleiter",
     "Namen über ihren Begleitern.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAME_ENEMY_PETS"},
    {"enemyguardiannames",
     "Feindliche Wächter",
     "Namen über ihren Wächtern.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAME_ENEMY_GUARDIANS"},
    {"enemytotemnames",
     "Feindliche Totems",
     "Namen über ihren Totems.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAME_ENEMY_TOTEMS"},
    {"friendlyplates",
     "Freundliche Namensplaketten",
     "Namen und Gesundheitsleisten über freundlichen Spielern und Kreaturen. Umschalt+V schaltet "
     "dies um, Strg+V beide Arten zusammen.",
     nullptr, nullptr, nullptr},
    {"friendlypetplates",
     "Freundliche Begleiter",
     "Plaketten über freundlichen Begleitern.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAMEPLATES_SHOW_FRIENDLY_PETS"},
    {"friendlyguardianplates",
     "Freundliche Wächter",
     "Plaketten über freundlichen Wächtern.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAMEPLATES_SHOW_FRIENDLY_GUARDIANS"},
    {"friendlytotemplates",
     "Freundliche Totems",
     "Plaketten über freundlichen Totems.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAMEPLATES_SHOW_FRIENDLY_TOTEMS"},
    {"enemyplates",
     "Feindliche Namensplaketten",
     "Namen und Gesundheitsleisten über feindlichen Spielern und Kreaturen. V schaltet dies um, "
     "Strg+V beide Arten zusammen.",
     nullptr, nullptr, nullptr},
    {"enemypetplates",
     "Feindliche Begleiter",
     "Plaketten über feindlichen Begleitern.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAMEPLATES_SHOW_ENEMY_PETS"},
    {"enemyguardianplates",
     "Feindliche Wächter",
     "Plaketten über feindlichen Wächtern.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAMEPLATES_SHOW_ENEMY_GUARDIANS"},
    {"enemytotemplates",
     "Feindliche Totems",
     "Plaketten über feindlichen Totems.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAMEPLATES_SHOW_ENEMY_TOTEMS"},
    {"nameplateoverlap",
     "Plaketten überlappen",
     "Plaketten über Einheiten, die in einer Reihe stehen, dürfen einander verdecken. Aus wird "
     "jede von den anderen weggeschoben.",
     nullptr, nullptr, "OPTION_TOOLTIP_UNIT_NAMEPLATES_ALLOW_OVERLAP"},
    {"nameplateclasscolours",
     "Plaketten in Klassenfarben",
     "Die Gesundheitsleiste eines feindlichen Spielers nach seiner Klasse färben.",
     nullptr, "SHOW_CLASS_COLOR_IN_V_KEY", "OPTION_TOOLTIP_SHOW_CLASS_COLOR_IN_V_KEY"},
    {"nameplatecastbar",
     "Zauberleiste an Plakette",
     "Unter der Namensplakette Eures Ziels zeigen, was es wirkt.",
     nullptr, nullptr, "OPTION_TOOLTIP_SHOW_TARGET_CASTBAR_IN_V_KEY"},
    {"combattext",
     "Schwebender Kampftext",
     "Die Hinweise unten über Eurem Charakter schweben lassen, wenn sie geschehen.",
     nullptr, nullptr, nullptr},
    {"combattextmode",
     "Richtung",
     "In welche Richtung der Text wandert.",
     "Aufwärts|Abwärts|Bogen", nullptr, nullptr},
    {"fctdodgeparrymiss",
     "Ausweich./Parier./Verfehl.",
     "Sagen, wenn ein Schlag gegen Euch verfehlte, und warum.",
     nullptr, "COMBAT_TEXT_SHOW_DODGE_PARRY_MISS_TEXT", nullptr},
    {"fctdamagereduction",
     "Schadensverringerung",
     "Wie viel eines Treffers Eure Rüstung, Widerstände und Absorptionen abfingen.",
     nullptr, "COMBAT_TEXT_SHOW_RESISTANCES_TEXT", nullptr},
    {"fctrepchanges",
     "Rufänderung",
     "Gewonnener und verlorener Ruf.",
     nullptr, "COMBAT_TEXT_SHOW_REPUTATION_TEXT", nullptr},
    {"fctreactives",
     "Reaktive Aktionen",
     "Wenn eine Fähigkeit, die einen Auslöser braucht - ein Ausweichen, ein kritischer Treffer - "
     "verfügbar wird.",
     nullptr, "COMBAT_TEXT_SHOW_REACTIVES_TEXT", nullptr},
    {"fctfriendlyhealers",
     "Namen befreundeter Heiler",
     "Wer Euch geheilt hat, mit dem Betrag.",
     nullptr, "COMBAT_TEXT_SHOW_FRIENDLY_NAMES_TEXT", nullptr},
    {"fctcombatstate",
     "Kampfstatus",
     "Ein Hinweis, wenn ein Kampf beginnt und wenn er endet.",
     nullptr, "COMBAT_TEXT_SHOW_COMBAT_STATE_TEXT", nullptr},
    {"fctcombopoints",
     "Combopunkte",
     "Jeder Combopunkt, wenn er erzielt wird.",
     nullptr, "COMBAT_TEXT_SHOW_COMBO_POINTS_TEXT", nullptr},
    {"fctlowmanahealth",
     "Niedr. Gesundh. & Mana",
     "Eine Warnung, wenn eines davon knapp wird.",
     nullptr, "COMBAT_TEXT_SHOW_LOW_HEALTH_MANA_TEXT", nullptr},
    {"fctenergygains",
     "Energiezuwachs",
     "Mana, Wut, Energie und Runenmacht, wenn sie hereinkommen.",
     nullptr, "COMBAT_TEXT_SHOW_ENERGIZE_TEXT", nullptr},
    {"fctperiodicenergygains",
     "Regelmäßiger Energiegewinn",
     "Dasselbe für das Rinnsal aus Regeneration und Effekten über Zeit.",
     nullptr, "COMBAT_TEXT_SHOW_PERIODIC_ENERGIZE_TEXT", nullptr},
    {"fcthonorgains",
     "Erhaltene Ehre",
     "Ehre, wenn sie verdient wird.",
     nullptr, "COMBAT_TEXT_SHOW_HONOR_GAINED_TEXT", nullptr},
    {"fctauras",
     "Auren",
     "Jeder Stärkungs- und Schwächungszauber, wenn er Euch trifft und wenn er verblasst.",
     nullptr, "COMBAT_TEXT_SHOW_AURAS_TEXT", nullptr},
    {"partybackground",
     "Gruppen-/Arenahintergrund",
     "Eine dunkle Fläche hinter den Gruppenfenstern.",
     nullptr, "SHOW_PARTY_BACKGROUND_TEXT", "OPTION_TOOLTIP_SHOW_PARTY_BACKGROUND"},
    {"hidepartyinraid",
     "Gruppe im Raid ausblenden",
     "Nur die Schlachtzugsfenster zeigen, während Ihr in einem seid.",
     nullptr, nullptr, "OPTION_TOOLTIP_HIDE_PARTY_INTERFACE"},
    {"partypets",
     "Begleiter der Gruppe",
     "Ein kleines Fenster unter jedem Gruppenmitglied für dessen Begleiter.",
     nullptr, "SHOW_PARTY_PETS_TEXT", "OPTION_TOOLTIP_SHOW_PARTY_PETS"},
    {"arenaframes",
     "Gegneranzeige in Arena",
     "Fenster für das andere Team in einem Arenakampf.",
     nullptr, "SHOW_ARENA_ENEMY_FRAMES_TEXT", "OPTION_TOOLTIP_SHOW_ARENA_ENEMY_FRAMES"},
    {"arenapets",
     "Ihre Begleiter",
     "Auch Fenster für die Begleiter des anderen Teams.",
     nullptr, nullptr, "OPTION_TOOLTIP_SHOW_ARENA_ENEMY_PETS"},
    {"fullsizefocus",
     "Größeres Fokusziel",
     "Das Fokusfenster in der Größe des Zielfensters statt kleiner zeichnen.",
     nullptr, "FULL_SIZE_FOCUS_FRAME_TEXT", "OPTION_TOOLTIP_FULL_SIZE_FOCUS_FRAME"},
    {"playerstatustext",
     "Am eigenen Fenster",
     "Die Zahlen auf Eure eigenen Gesundheits- und Machtleisten schreiben.",
     nullptr, nullptr, nullptr},
    {"petstatustext",
     "Am Fenster des Begleiters",
     "Dasselbe auf den Leisten Eures Begleiters.",
     nullptr, nullptr, nullptr},
    {"partystatustext",
     "An den Gruppenfenstern",
     "Dasselbe auf den Leisten jedes Gruppenmitglieds.",
     nullptr, nullptr, nullptr},
    {"targetstatustext",
     "Am Zielfenster",
     "Dasselbe auf den Leisten Eures Ziels.",
     nullptr, nullptr, nullptr},
    {"statustextpercent",
     "Prozentsatz anzeigen",
     "Prozente statt der Beträge.",
     nullptr, "STATUS_TEXT_PERCENT", nullptr},
    {"xpbartext",
     "An der Erfahrungsleiste",
     "Eure Erfahrung auf die Leiste schreiben, nicht nur wenn die Maus darüber ist.",
     nullptr, nullptr, "OPTION_TOOLTIP_XP_BAR"},
    {"threatwarning",
     "Bedrohungswarnung",
     "Wann das Zielfenster aufblinkt, wenn sich eine Kreatur Euch zuwendet.",
     "Nie|In Dungeons und Schlachtzügen|In der Gruppe|Immer", nullptr, nullptr},
    {"threatpercent",
     "Bedrohung in Prozent",
     "Zeigen, wie nah Ihr daran seid, Euer Ziel von dem abzuziehen, der es hat.",
     nullptr, "SHOW_NUMERIC_THREAT", "OPTION_TOOLTIP_SHOW_NUMERIC_THREAT"},
    {"threatsounds",
     "Bedrohungs-Sounds abspielen",
     "Einen Ton abspielen, wenn Eure Bedrohung die nächste Stufe erreicht.",
     nullptr, "PLAY_AGGRO_SOUNDS", "OPTION_TOOLTIP_PLAY_AGGRO_SOUNDS"},
    {"animatedportraits",
     "Bewegte Porträts",
     "Die Gesichter auf Eurem Fenster, dem Eures Ziels, Eurer Gruppe und den übrigen als "
     "lebendige, bewegte Modelle statt als Standbilder zeigen.",
     nullptr, nullptr, nullptr},
    {"mutesound",
     "Allen Ton stummschalten",
     "Alles auf einmal stummschalten. Die Lautsprecherschaltfläche neben der Minikarte ist "
     "derselbe Schalter.",
     nullptr, nullptr, nullptr},
    {"mastervolume",
     "Gesamtlautstärke",
     "Ein Regler über allem darunter.",
     nullptr, "MASTER_VOLUME", nullptr},
    {"soundinbackground",
     "Sound im Hintergrund",
     "Weiterspielen, wenn ein anderes Fenster den Fokus hat.",
     nullptr, "ENABLE_BGSOUND", nullptr},
    {"enablemusic",
     "Musik",
     "Aus bringt die Musik zum Schweigen, ohne die Lautstärke darunter zu ändern.",
     nullptr, "ENABLE_MUSIC", nullptr},
    {"musicvolume",
     "Musiklautstärke",
     "Die Musik der Zone.",
     nullptr, nullptr, nullptr},
    {"loopmusic",
     "Ohne Pausen abspielen",
     "Das nächste Stück der Zone gleich beginnen, statt die Stille zu lassen, die ihre Musik "
     "zwischen die Stücke setzt (Sound_ZoneMusicNoDelay).",
     nullptr, nullptr, nullptr},
    {"enableambience",
     "Umgebungsgeräusche",
     "Aus bringt die eigenen Geräusche der Welt zum Schweigen, ohne die Lautstärken darunter zu "
     "ändern.",
     nullptr, "ENABLE_AMBIENCE", nullptr},
    {"ambientvolume",
     "Umgebungslautstärke",
     "Wind, Wasser, Vögel und die übrigen eigenen Geräusche der Welt.",
     nullptr, nullptr, nullptr},
    {"npcvoicevolume",
     "NSC-Stimmen",
     "Grüße, Abschiede und Questreden der Leute, mit denen Ihr sprecht.",
     nullptr, nullptr, nullptr},
    {"characterspeech",
     "Emote-Sounds",
     "Ächzen, Grüße und Emotes Eures eigenen Charakters.",
     nullptr, "ENABLE_EMOTE_SOUNDS", nullptr},
    {"errorspeech",
     "Audiofehlermeldungen",
     "Euer Charakter sagt, warum eine Aktion abgelehnt wurde - \"Das kann ich noch nicht\", "
     "\"Nicht genug Wut\".",
     nullptr, "ENABLE_ERROR_SPEECH", nullptr},
    {"enablesoundeffects",
     "Soundeffekte",
     "Aus bringt alle Effekte darunter auf einmal zum Schweigen.",
     nullptr, "ENABLE_SOUNDFX", nullptr},
    {"effectsvolume",
     "Alle Effekte",
     "Ein Regler über den sieben darunter. Dies ist der Regler, den das eigene Soundfenster des "
     "Spiels Soundeffekte nannte.",
     nullptr, nullptr, nullptr},
    {"uivolume",
     "Oberfläche",
     "Klicks, Taschengeräusche und Fenstergeräusche.",
     nullptr, nullptr, nullptr},
    {"combatvolume",
     "Kampf",
     "Waffenschwünge, Treffer, Blocken und Parieren.",
     nullptr, nullptr, nullptr},
    {"spellvolume",
     "Zauber",
     "Zauberwirken, Einschläge und landende Stärkungen.",
     nullptr, nullptr, nullptr},
    {"movementvolume",
     "Bewegung",
     "Springen, Landen, Schwimmen und Rüstungsrascheln.",
     nullptr, nullptr, nullptr},
    {"footstepvolume",
     "Schritte",
     "Eure und fremde Schritte, nach dem Boden darunter.",
     nullptr, nullptr, nullptr},
    {"mountvolume",
     "Reittiere",
     "Hufschläge, Flügelschläge und Rufe der Reittiere.",
     nullptr, nullptr, nullptr},
    {"activityvolume",
     "Tätigkeiten",
     "Angeln, Bergbau, Schmieden und die übrige Welt bei der Arbeit.",
     nullptr, nullptr, nullptr},
    {"chatboxvisible",
     "Chateingabe immer sichtbar",
     "Das Chateingabefeld auf dem Bildschirm lassen, damit es angeklickt werden kann. Aus "
     "erscheint es mit Eingabe und verschwindet beim Senden.",
     nullptr, nullptr, nullptr},
    {"clicktofocus",
     "Klicken zum Schreiben",
     "Ein Klick irgendwo ins Chatfenster setzt den Cursor ins Eingabefeld, nicht nur ein Klick auf "
     "das Feld.",
     nullptr, nullptr, "OPTION_TOOLTIP_CHAT_WHOLE_WINDOW_CLICKABLE"},
    {"chatmousescroll",
     "Mit dem Mausrad scrollen",
     "Das Rad scrollt das Chatfenster, während die Maus darüber ist.",
     nullptr, nullptr, "OPTION_TOOLTIP_CHAT_MOUSE_WHEEL_SCROLL"},
    {"removechatdelay",
     "Links ohne Verzögerung",
     "Gegenstands- und Spielerlinks reagieren, sobald die Maus darüber ist, statt nach einer "
     "Pause.",
     nullptr, nullptr, "OPTION_TOOLTIP_REMOVE_CHAT_DELAY"},
    {"chattimestamps",
     "Zeitstempel",
     "Die Uhrzeit vor jede Zeile setzen.",
     "Keine|3:45|3:45:12|3:45 pm|3:45:12 pm", nullptr, nullptr},
    {"spamfilter",
     "Wiederholungen weglassen",
     "Eine Zeile, die derselbe Spieler einen Moment später erneut einfügt, wird nicht gezeigt.",
     nullptr, nullptr, nullptr},
    {"lootrollmessages",
     "Detaillierte Plünderanzeige",
     "Den Wurf jedes Spielers auf einen Gegenstand nennen, nicht nur, wer ihn gewann.",
     nullptr, "SHOW_LOOT_SPAM", "OPTION_TOOLTIP_SHOW_LOOT_SPAM"},
    {"guildmemberalerts",
     "Gildenmitgliederalarm",
     "Eine Zeile, wenn sich ein Gildenmitglied an- oder abmeldet.",
     nullptr, "GUILDMEMBER_ALERT", "OPTION_TOOLTIP_GUILDMEMBER_ALERT"},
    {"joingeneral",
     "Allgemein",
     "Alle in Eurer aktuellen Zone.",
     nullptr, nullptr, nullptr},
    {"jointrade",
     "Handel",
     "Kaufen und Verkaufen. Zwischen den Hauptstädten geteilt und nur dort zu hören.",
     nullptr, nullptr, nullptr},
    {"joinlocaldefense",
     "LokaleVerteidigung",
     "Warnungen, wenn Orte in Eurer Zone angegriffen werden.",
     nullptr, nullptr, nullptr},
    {"joinlfg",
     "SucheNachGruppe",
     "Leute für Dungeons, Quests und Schlachtzüge finden.",
     nullptr, nullptr, nullptr},
    {"joinlocal",
     "Lokal",
     "Der Lokal-Kanal des Realms, auf Realms, die einen anbieten.",
     nullptr, nullptr, nullptr},
    {"joinguildrecruitment",
     "Gildenrekrutierung",
     "Gilden, die Mitglieder suchen, und Spieler, die eine Gilde suchen.",
     nullptr, nullptr, "OPTION_TOOLTIP_AUTO_JOIN_GUILD_CHANNEL"},
    {"chatbubbles",
     "Sprechblasen",
     "Was in der Nähe gesagt und gerufen wird, über dem Kopf des Sprechers schweben lassen.",
     nullptr, "CHAT_BUBBLES_TEXT", nullptr},
    {"chatbubblesparty",
     "Sprechblasen für Gruppenchat",
     "Auch Gruppen- und Schlachtzugschat über dem Kopf des Sprechers schweben lassen.",
     nullptr, "PARTY_CHAT_BUBBLES_TEXT", nullptr},
    {"autoloot",
     "Schnell-Plündern",
     "Alles aus einer Leiche oder Truhe nehmen, ohne das Beutefenster zu öffnen.",
     nullptr, "AUTO_LOOT_DEFAULT_TEXT", nullptr},
    {"autolootkey",
     "Taste für Schnell-Plündern",
     "Diese beim Öffnen einer Leiche halten, um für diese eine Leiche das Gegenteil der "
     "Einstellung oben zu tun.",
     "Keine|Alt|Strg|Umschalt", "AUTO_LOOT_KEY_TEXT", nullptr},
    {"lootatmouse",
     "Beutefenster an der Maus",
     "Das Beutefenster dort öffnen, wo die Maus ist, statt an seinem üblichen Platz.",
     nullptr, nullptr, "OPTION_TOOLTIP_LOOT_UNDER_MOUSE"},
    {"autosellgrey",
     "Plunder verkaufen",
     "Beim Öffnen eines Händlers wird jeder Gegenstand schlechter Qualität - die grauen - aus "
     "Rucksack und Taschen verkauft und das Geld gutgeschrieben. Nichts anderer Farbe wird "
     "angerührt. Es gibt keine Rückfrage und kein Rückgängig, ein absichtlich behaltener grauer "
     "Gegenstand geht also mit.",
     nullptr, nullptr, nullptr},
    {"autorepair",
     "Automatisch reparieren",
     "Beim Öffnen eines Händlers, der repariert, wird alles Beschädigte, das Ihr tragt, aus Eurem "
     "eigenen Geld repariert - nie aus der Gildenbank. Könnt Ihr die ganze Rechnung nicht "
     "bezahlen, wird nichts repariert, und es wird gesagt.",
     nullptr, nullptr, nullptr},
    {"groundclearstarget",
     "Bodenklick löst Ziel",
     "Ein Klick ins Leere lässt Euer Ziel los. Aus bleibt das Ziel, bis Ihr ein anderes wählt - "
     "was das Spiel fixiertes Zielen nennt.",
     nullptr, nullptr, "OPTION_TOOLTIP_GAMEFIELD_DESELECT"},
    {"autodismount",
     "Autom. Absitzen (Flug)",
     "Zaubern auf einem fliegenden Reittier lässt Euch zuerst absitzen, statt den Zauber "
     "abzulehnen.",
     nullptr, "AUTO_DISMOUNT_FLYING_TEXT", "OPTION_TOOLTIP_AUTO_DISMOUNT_FLYING"},
    {"autoclearafk",
     "Abwesend endet bei Bewegung",
     "Bewegen oder Sprechen beendet Euren Abwesenheitsstatus.",
     nullptr, nullptr, "OPTION_TOOLTIP_CLEAR_AFK"},
    {"blocktrades",
     "Handel blocken",
     "Handelsanfragen werden abgelehnt, bevor ihr Fenster sich öffnet, und der Chat sagt es.",
     nullptr, "BLOCK_TRADES", "OPTION_TOOLTIP_BLOCK_TRADES"},
    {"instantquesttext",
     "Questtext sofort anzeigen",
     "Questtext erscheint ganz, statt Zeile für Zeile aufgedeckt zu werden.",
     nullptr, "SHOW_QUEST_FADING_TEXT", "OPTION_TOOLTIP_SHOW_QUEST_FADING"},
    {"autotrackquests",
     "Quest bei Annahme verfolgen",
     "Eine neue Quest kommt sofort in die Zielverfolgung.",
     nullptr, nullptr, "OPTION_TOOLTIP_AUTO_QUEST_WATCH"},
    {"widerquesttracker",
     "Breitere Zielverfolgung",
     "Der Zielverfolgung mehr Breite geben, damit lange Ziele in eine Zeile passen.",
     nullptr, nullptr, "OPTION_TOOLTIP_WATCH_FRAME_WIDTH"},
    {"showcloak",
     "Umhang zeigen",
     "Euren Umhang so tragen, dass andere ihn sehen. Der Server behält dies, es folgt also dem "
     "Charakter, nicht dem Client.",
     nullptr, "SHOW_CLOAK", nullptr},
    {"showhelm",
     "Helm zeigen",
     "Dasselbe für den Helm.",
     nullptr, "SHOW_HELM", nullptr},
    {"checkforupdates",
     "Nach neuen Versionen suchen",
     "Beim Start einmal bei GitHub fragen, ob ein neueres WoWee erschienen ist, und es auf dem "
     "Anmeldebildschirm sagen. Es wird nichts heruntergeladen oder installiert, und nichts über "
     "Euch wird gesendet.",
     nullptr, nullptr, nullptr},
};

struct Pair {
    const char* en;
    const char* de;
};

constexpr Pair kCategories[] = {
    {"Graphics", "Grafik"},
    {"Shadows", "Schatten"},
    {"Detail", "Details"},
    {"Water", "Wasser"},
    {"Ray Tracing", "Raytracing"},
    {"Debug Graphics", "Debug: Grafik"},
    {"Debug Time", "Debug: Zeit"},
    {"Debug Lights", "Debug: Lichter"},
    {"Debug Glow", "Debug: Leuchten"},
    {"Debug Shadows", "Debug: Schatten"},
    {"Upscaling", "Hochskalierung"},
    {"Grass", "Gras"},
    {"Display", "Anzeige"},
    {"Camera", "Kamera"},
    {"Interface", "Oberfläche"},
    {"Minimap", "Minikarte"},
    {"Action Bars", "Aktionsleisten"},
    {"HUD", "Anzeigen"},
    {"Combat", "Kampf"},
    {"Names", "Namen"},
    {"Nameplates", "Namensplaketten"},
    {"Combat Text", "Kampftext"},
    {"Unit Frames", "Einheitenfenster"},
    {"Sound", "Sound"},
    {"Sound Effects", "Soundeffekte"},
    {"Chat", "Chat"},
    {"Gameplay", "Spiel"},
};

constexpr Pair kSections[] = {
    {"Always", "Immer"},
    {"Ambience", "Umgebung"},
    {"Anti-aliasing", "Kantenglättung"},
    {"Appearance", "Aussehen"},
    {"Arena", "Arena"},
    {"Atmosphere", "Atmosphäre"},
    {"Attacking", "Angreifen"},
    {"Bags", "Taschen"},
    {"Bubbles", "Sprechblasen"},
    {"Buffs", "Stärkungszauber"},
    {"Casting", "Zaubern"},
    {"Channels to join", "Kanäle beitreten"},
    {"Character", "Charakter"},
    {"Character sheet", "Charakterfenster"},
    {"Controller", "Controller"},
    {"Controls", "Steuerung"},
    {"Effects", "Effekte"},
    {"Extra bars", "Zusätzliche Leisten"},
    {"Eye adaptation", "Augenanpassung"},
    {"FSR 3 tuning", "FSR-3-Feinabstimmung"},
    {"Focus", "Fokus"},
    {"Ground cover", "Bodenbedeckung"},
    {"Ground cover (experimental)", "Bodenbedeckung (experimentell)"},
    {"Health and power numbers", "Zahlen zu Gesundheit und Macht"},
    {"Highly experimental", "Sehr experimentell"},
    {"Input", "Eingabe"},
    {"Intensity and reach", "Stärke und Reichweite"},
    {"Interiors, ray traced", "Innenräume, Raytracing"},
    {"Light", "Licht"},
    {"Lighting", "Beleuchtung"},
    {"Limit", "Grenze"},
    {"Looting", "Plündern"},
    {"Messages", "Nachrichten"},
    {"Mode", "Modus"},
    {"Mouse", "Maus"},
    {"Music", "Musik"},
    {"Nameplates", "Namensplaketten"},
    {"Names over heads", "Namen über Köpfen"},
    {"Output", "Ausgabe"},
    {"Over your character", "Über Eurem Charakter"},
    {"Party", "Gruppe"},
    {"Portraits", "Porträts"},
    {"Quality", "Qualität"},
    {"Quests", "Quests"},
    {"Ray traced", "Raytracing"},
    {"Rays a pixel", "Strahlen je Pixel"},
    {"Scale", "Größe"},
    {"Screen", "Bildschirm"},
    {"Screen effects", "Bildschirmeffekte"},
    {"Shadows", "Schatten"},
    {"Sky", "Himmel"},
    {"Surfaces", "Oberflächen"},
    {"Target", "Ziel"},
    {"Textures", "Texturen"},
    {"Threat", "Bedrohung"},
    {"Time of day", "Tageszeit"},
    {"Tooltips", "Tooltips"},
    {"Trackers", "Anzeigen"},
    {"Updates", "Aktualisierungen"},
    {"View", "Ansicht"},
    {"Voices", "Stimmen"},
    {"Water", "Wasser"},
    {"What glows", "Was leuchtet"},
    {"Windows", "Fenster"},
};

constexpr Pair kRtIgnoredGerman[] = {
    {"dbgterrainshadow",
     "Mit Raytracing ersetzt der verfolgte Sonnenschatten (oder die Schattenkarte) die gemalten "
     "Schatten des Geländes."},
    {"dbgblobshadow",
     "Mit Raytracing werfen Einheiten verfolgte Schatten, und die runden Schatten unter ihnen "
     "werden nicht gezeichnet."},
};

const char* lookup(const Pair* table, std::size_t n, const std::string& en) {
    for (std::size_t i = 0; i < n; ++i) {
        if (en == table[i].en) return table[i].de;
    }
    return nullptr;
}

}  // namespace

const SettingGerman* germanSettingText(const std::string& key) {
    for (const auto& row : kGerman) {
        if (key == row.key) return &row;
    }
    return nullptr;
}

const SettingGerman* germanSettingTable(std::size_t& count) {
    count = std::size(kGerman);
    return kGerman;
}

const char* germanCategoryName(const std::string& category) {
    return lookup(kCategories, std::size(kCategories), category);
}

const char* germanSectionName(const std::string& section) {
    return lookup(kSections, std::size(kSections), section);
}

SettingText settingText(const SettingDesc& desc, bool german, const GlobalStringLookup& globals) {
    SettingText t{desc.label, desc.tooltip, desc.choices, desc.category, desc.section};
    if (!german) return t;
    t.category = categoryText(t.category, true);
    t.section = sectionText(t.section, true);
    const SettingGerman* de = germanSettingText(desc.key);
    if (!de) return t;
    t.label = de->label;
    t.tooltip = de->tooltip;
    if (de->choices) t.choices = de->choices;
    // The interface's own words where it has them and is there to say them.
    if (globals) {
        if (de->gsLabel) {
            std::string s = globals(de->gsLabel);
            if (!s.empty()) t.label = std::move(s);
        }
        if (de->gsTooltip) {
            std::string s = globals(de->gsTooltip);
            if (!s.empty()) t.tooltip = std::move(s);
        }
    }
    return t;
}

std::string categoryText(const std::string& category, bool german) {
    if (german) {
        if (const char* de = germanCategoryName(category)) return de;
    }
    return category;
}

std::string sectionText(const std::string& section, bool german) {
    if (german && !section.empty()) {
        if (const char* de = germanSectionName(section)) return de;
    }
    return section;
}

const char* rayTracingIgnoredText(const std::string& key, bool german) {
    if (german) {
        if (const char* de = lookup(kRtIgnoredGerman, std::size(kRtIgnoredGerman), key)) return de;
    }
    return rayTracingIgnoredReason(key);
}

}  // namespace ui
}  // namespace wowee
