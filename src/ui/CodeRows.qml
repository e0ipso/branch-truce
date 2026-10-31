// One presentation-only row-spacing policy for editors and snapshot readers.
import org.kde.kirigami as Kirigami

TextBlocks {
    // The pinned fixed font's laid-out row plus this native gap is the
    // reference pitch. TextBlocks applies it once, including wrapped rows.
    rowSpacing: Kirigami.Units.mediumSpacing
}
