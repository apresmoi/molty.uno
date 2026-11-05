export class MoltyStage {
  constructor(px?: number);
  el: HTMLCanvasElement;
  setSize(px: number): void;
  blob(): void;
  word(w: string): void;
}
