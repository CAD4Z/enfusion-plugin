import type { EddsPreview } from '../../mods/texture/edds';

/** Byte analysis reads one plain surface: neither an HDR display mapping nor a face of a cube. */
export function byteAnalysisApplies(preview: EddsPreview): boolean {
  return preview.displayMapping === undefined && preview.faces === undefined && preview.facesOmitted === undefined;
}

/** Faces are decoded from the same inspected mip; selection never projects the source in JS. */
export function textureSurfaceControls(preview: EddsPreview, select: (surface: EddsPreview) => void): HTMLElement {
  const controls = document.createElement('div');
  if (preview.displayMapping !== undefined) {
    const mapping = document.createElement('p');
    mapping.textContent = `HDR display: ${preview.displayMapping}. Linear values remain in the texture; byte analysis is unavailable for this display.`;
    controls.append(mapping);
  }
  if (preview.facesOmitted !== undefined) {
    const omitted = document.createElement('p');
    omitted.textContent = preview.facesOmitted;
    controls.append(omitted);
  }
  if (preview.faces !== undefined) {
    const label = document.createElement('label');
    label.textContent = 'Cube face ';
    const faces = document.createElement('select');
    ['+X', '-X', '+Y', '-Y', '+Z', '-Z'].forEach((name, index) => {
      const option = document.createElement('option');
      option.value = String(index);
      option.textContent = name;
      faces.append(option);
    });
    faces.addEventListener('change', () => {
      const rgba = preview.faces?.[Number(faces.value)];
      if (rgba !== undefined) select({ ...preview, rgba });
    });
    label.append(faces);
    controls.append(label);
  }
  return controls;
}
